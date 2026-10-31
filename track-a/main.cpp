// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads does everything in two phases.
//
// Phase 1, parse. The log is memory-mapped and cut into line-aligned ranges,
// one per thread. The timestamp is always 27 bytes and the 72-hour span is
// fixed, so a line's hour index comes from its day and hour digits alone. One
// 64-byte scan after the timestamp finds the method's end, the endpoint's end
// and the line's end. Endpoints are interned in one lock-free table shared by
// all threads, and each line files a 4-byte record (latency, endpoint id, 5xx
// flag) under its hour.
//
// Between the phases the last thread to arrive sorts the names, so each
// endpoint id gets its rank in output order.
//
// Phase 2, hours. Threads claim hours in order. An hour's ~43k latencies are
// counting-sorted by endpoint rank into per-thread scratch and three
// selections per group give the percentiles. Each group's numbers are kept
// (24 bytes a line) along with the exact size the hour's text will take, since
// a number's width is known without formatting it.
//
// Phase 3, text. The last thread into the barrier turns the sizes into
// offsets, sizes the output file and maps it; then every thread formats its
// hours straight into the file. No hour waits for another and no write call
// queues on the file's lock, and the output's pages fill only after the
// records have been released.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;

constexpr int HOURS = 72;  // 2026-10-30T18:00Z to 2026-11-02T17:59:59Z
// A record packs the latency above a 13-bit key: a 12-bit endpoint id and
// the 5xx flag. Latencies of 2^19 us or more (about 3% of lines) do not fit
// and are kept on a side list with their key instead.
constexpr u32 KEY_BITS = 13, ERR = 1u << 12, ID_MASK = ERR - 1, PACKED_MAX = 1u << 19;

// Bit i is set when byte i of the 32 at p equals c.
inline u32 eq32(const char* p, char c) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}

// The number of decimal digits in x.
inline u32 width(u32 x) { return x < 10 ? 1 : 1 + width(x / 10); }

// The value of the len (1 to 10) digits ending just before end, with no loop:
// the 16 bytes before end, minus '0' and with the bytes ahead of the number
// zeroed, are combined 2, 4 and then 8 digits at a time. (A digit loop here
// cost about 5% more instructions in all.)
inline u32 decode(const char* end, u32 len) {
    __m128i v = _mm_subs_epu8(_mm_loadu_si128((const __m128i*)(end - 16)), _mm_set1_epi8('0'));
    __m128i lane = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
    v = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));  // pairs (10, 1)
    v = _mm_madd_epi16(v, _mm_set1_epi32(0x00010064));  // pairs (100, 1)
    v = _mm_madd_epi16(_mm_packus_epi32(v, v), _mm_set1_epi32(0x00012710));
    return u32(_mm_cvtsi128_si32(v)) * u64(100000000) + u32(_mm_extract_epi32(v, 1));
}

// Puts the k-th smallest of a[0, n) at a[k], the smaller ones before it,
// without a branch on the data: each step partitions around the median of
// three by Lomuto's scheme with an unconditional swap, the boundary moving
// by the comparison's result. (std::nth_element mispredicted at about every
// other element.) Many equal values could make a round shrink the range by
// only one, so after 40 rounds the library's selection finishes.
inline void select(u32* a, u32 n, u32 k) {
    for (int round = 0; n > 1; ++round) {
        if (round == 40) return std::nth_element(a, a + k, a + n);
        u32 m = n / 2;  // sort a[0], a[m], a[n - 1], then the median is the pivot
        if (a[m] < a[0]) std::swap(a[m], a[0]);
        if (a[n - 1] < a[m]) std::swap(a[n - 1], a[m]);
        if (a[m] < a[0]) std::swap(a[m], a[0]);
        std::swap(a[m], a[n - 1]);
        u32 pivot = a[n - 1], lt = 0;
        for (u32 i = 0; i + 1 < n; ++i) {
            u32 x = a[i];
            a[i] = a[lt], a[lt] = x, lt += x < pivot;
        }
        std::swap(a[lt], a[n - 1]);
        if (k == lt) return;
        if (k < lt) n = lt;
        else a += lt + 1, k -= lt + 1, n -= lt + 1;
    }
}

// The first c at or after s; one is known to follow.
inline const char* next_byte(const char* s, char c) {
    for (;; s += 32)
        if (u32 m = eq32(s, c)) return s + __builtin_ctz(m);
}

// Endpoint names to ids, one table for all threads. Open addressing on a CRC
// of the name's first and last eight bytes (masked when the name is shorter;
// the rest of the line keeps both loads in bounds), and a name's id is its
// slot. A new name is copied into the arena, so the names compared on every
// line sit together, and then published with one compare-and-swap; a thread
// that loses the race for the slot compares against the winner's name.
struct Names {
    static constexpr u32 SLOTS = 1 << 12, ARENA = SLOTS * 128;  // a seed has 1,842
    std::atomic<u64> slot[SLOTS];  // length << 32 | arena offset, 0 if free
    std::atomic<u32> used{0};
    char arena[ARENA + 64];  // 64 spare bytes keep the last compare in bounds

    std::string_view name(u64 v) const { return {arena + u32(v), size_t(v >> 32)}; }

    u32 operator()(std::string_view s) {
        u64 a, b = 0;
        memcpy(&a, s.data(), 8);
        if (s.size() >= 8) memcpy(&b, s.data() + s.size() - 8, 8);
        else a &= (u64(1) << 8 * s.size()) - 1;
        for (u32 i = _mm_crc32_u64(_mm_crc32_u64(s.size(), a), b), end = i + SLOTS; i != end; ++i) {
            u64 v = slot[i % SLOTS].load(std::memory_order_acquire);
            if (v == 0) {
                u32 at = used.fetch_add(s.size());
                if (at + s.size() > ARENA) _exit(2);
                memcpy(arena + at, s.data(), s.size());
                if (slot[i % SLOTS].compare_exchange_strong(v, u64(s.size()) << 32 | at)) return i % SLOTS;
            }
            if (same(name(v), s)) return i % SLOTS;
        }
        _exit(2);  // more names than slots
    }

    // 64 bytes can always be read from either name (the rest of the line and
    // the spare page follow one, the arena's spare bytes the other), so a name
    // of up to 64 bytes (94% of lines) is compared in two steps, not memcmp.
    static bool same(std::string_view x, std::string_view s) {
        if (x.size() != s.size() || s.size() > 64) return x == s;
        auto eq = [&](int i) { return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)(x.data() + i)), _mm256_loadu_si256((const __m256i*)(s.data() + i))))); };
        return (~(eq(0) | u64(eq(32)) << 32) & (~u64(0) >> (64 - s.size()))) == 0;
    }
};
static Names endpoints;  // zero pages until written

// One hour's records from one thread, in a chain of 16 KB chunks carved in
// order from a region the thread maps up front (one mmap, so no lock is
// shared). The output file's pages count toward peak memory as it is written,
// so chunks are handed back while it grows: see Parser::release.
struct Chunk {
    static constexpr u32 CAP = (16384 - 16) / 4;
    Chunk* prev;
    u32 n, hour;
    u32 rec[CAP];
};

struct Parser {
    Chunk* last[HOURS] = {};
    // (latency, key) of the long ones.
    std::vector<std::pair<u32, u32>> wide[HOURS];
    // next: where the next chunk is carved; kept: first one not yet released.
    Chunk *next = nullptr, *kept = nullptr;

    // Room for a log of `size` bytes: a line is at least 40 bytes, and each
    // hour may leave one chunk part full. Pages are charged only once written.
    void map(size_t size) {
        size_t bytes = (size / 40 / Chunk::CAP + 1 + HOURS) * sizeof(Chunk);
        next = kept = (Chunk*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }

    void run(const char* p, const char* end) {
        while (p < end) {
            // YYYY-MM-DDTHH: month's last digit at 6, day at 8-9, hour at 11-12.
            int day = (p[8] - '0') * 10 + (p[9] - '0'), hour = (p[11] - '0') * 10 + (p[12] - '0');
            int h = (p[6] == '0' ? day - 30 : day + 1) * 24 + hour - 18;

            // Spaces and newlines in the 64 bytes after the timestamp. Only an
            // endpoint near its 125-byte maximum pushes past the window.
            const char* m = p + 28;
            u64 sp = eq32(m, ' ') | u64(eq32(m + 32, ' ')) << 32;
            u64 nl = eq32(m, '\n') | u64(eq32(m + 32, '\n')) << 32;
            const char* name = m + __builtin_ctzll(sp) + 1;
            const char* name_end = nl ? m + __builtin_ctzll(sp & (sp - 1)) : next_byte(name, ' ');
            const char* eol = nl ? m + __builtin_ctzll(nl) : next_byte(name_end + 5, '\n');
            u32 key = endpoints(std::string_view(name, name_end - name)) | (name_end[1] == '5' ? ERR : 0);
            u32 latency = decode(eol, eol - name_end - 5);  // after " SSS "
            p = eol + 1;

            if (__builtin_expect(latency >= PACKED_MAX, 0)) {
                wide[h].push_back({latency, key});
                continue;
            }
            Chunk*& c = last[h];
            if (!c || c->n == Chunk::CAP) next->prev = c, next->n = 0, next->hour = h, c = next++;
            c->rec[c->n++] = latency << KEY_BITS | key;
        }
    }

    // The log is in arrival order, so chunks are carved roughly hour by hour
    // and hours finish in the same order: the run of chunks at the front whose
    // hours are all finished grows steadily. Hand it back once it reaches 64
    // KB, in one madvise, instead of one call (and TLB shootdown) per chunk;
    // larger batches left about 1 MB more resident at the peak.
    void release(const std::atomic<bool>* finished) {
        Chunk* c = kept;
        while (c < next && finished[c->hour]) ++c;
        if (c - kept >= 4 || (c == next && c > kept)) {
            madvise(kept, (c - kept) * sizeof(Chunk), MADV_DONTNEED);
            kept = c;
        }
    }

    // Calls f(endpoint rank, key, latency) for every record of hour h.
    template <class F>
    void each(int h, const u32* rank, F&& f) const {
        for (const Chunk* c = last[h]; c; c = c->prev)
            for (u32 i = 0; i < c->n; ++i) f(rank[c->rec[i] & ID_MASK], c->rec[i] & ERR, c->rec[i] >> KEY_BITS);
        for (auto [latency, key] : wide[h]) f(rank[key & ID_MASK], key & ERR, latency);
    }
};

int main() {
    // With transparent huge pages, a chunk region written only in part could be
    // backed by whole 2 MB pages and count against peak memory.
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    const int nt = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
    int in = open("/data/requests.log", O_RDONLY);
    int out = open("/out/summary.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) return perror("open"), 1;

    // Map the log over one spare zero page, so the 64-byte scans stay in bounds.
    size_t size = lseek(in, 0, SEEK_END);
    auto* log = (char*)mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (size && mmap(log, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, in, 0) == MAP_FAILED) return perror("mmap"), 1;
    // Line-aligned pieces, eight per thread, claimed in order: a thread that
    // starts late or shares its core takes fewer.
    const int pieces = 8 * nt;
    std::vector<size_t> cut(pieces + 1, size);
    cut[0] = 0;
    for (int k = 1; k < pieces; ++k)
        for (cut[k] = std::max(cut[k - 1], size * k / pieces); cut[k] < size && log[cut[k] - 1] != '\n';) ++cut[k];
    std::atomic<int> claim_piece{0};

    // Between the phases, endpoints are ranked in unsigned byte order (a
    // proper prefix first), which is the order lines are printed in an hour.
    std::vector<Parser> parsers(nt);
    std::vector<std::string_view> names;
    std::vector<u32> rank(Names::SLOTS);  // endpoint id -> rank
    u32 most = 0;  // records in the largest hour
    std::barrier ranked(nt, [&]() noexcept {
        for (int h = 0; h < HOURS; ++h) {
            u32 n = 0;
            for (auto& ps : parsers) ps.each(h, rank.data(), [&](u32, u32, u32) { ++n; });
            most = std::max(most, n);
        }
        std::vector<std::pair<std::string_view, u32>> by_name;
        for (u32 id = 0; id < Names::SLOTS; ++id)
            if (u64 v = endpoints.slot[id]) by_name.push_back({endpoints.name(v), id});
        std::sort(by_name.begin(), by_name.end());
        for (auto [name, id] : by_name) rank[id] = names.size(), names.push_back(name);
    });

    // An hour is finished once its records are copied out; one thread at a
    // time then releases whatever chunks that frees.
    std::vector<std::atomic<bool>> finished(HOURS);
    std::atomic_flag releasing;
    std::atomic<int> claim{0}, claim_text{0};
    struct Line {
        u32 e, n, p50, p95, p99, errors;
    };
    auto ppm = [](const Line& l) { return u32(u64(l.errors) * 1000000 / l.n); };
    std::vector<std::vector<Line>> lines(HOURS);
    std::vector<u64> at(HOURS + 1);  // an hour's text size, then its offset
    char* text = nullptr;
    std::barrier sized(nt, [&]() noexcept {
        // Every hour is finished: whatever chunks the last releases left go
        // back now, before the output's pages start to count.
        for (auto& ps : parsers) ps.release(finished.data());
        u64 total = 0;
        for (int h = 0; h < HOURS; ++h) total += std::exchange(at[h], total);
        at[HOURS] = total;
        if (total == 0) return;
        if (ftruncate(out, total) != 0) _exit(1);
        text = (char*)mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, out, 0);
        if (text == MAP_FAILED) _exit(1);
    });

    std::vector<std::thread> pool;
    for (int t = 0; t < nt; ++t) pool.emplace_back([&, t] {
        parsers[t].map(size);
        for (int k; (k = claim_piece++) < pieces;) parsers[t].run(log + cut[k], log + cut[k + 1]);
        ranked.arrive_and_wait();

        {  // the scratch goes before the text phase, where the output counts
        const u32 ne = names.size();
        // Sized once: a buffer that grows is remapped, which stalls every
        // thread's page faults.
        std::vector<u32> start(ne + 1), errors(ne), latency(most);
        for (int h; (h = claim++) < HOURS;) {
            // Counting sort of the hour's latencies by endpoint rank: count into
            // start[e + 1], prefix, then place each latency at its group's cursor.
            std::fill(start.begin(), start.end(), 0);
            std::fill(errors.begin(), errors.end(), 0);
            for (auto& ps : parsers) ps.each(h, rank.data(), [&](u32 e, u32 err, u32) { ++start[e + 1], errors[e] += err != 0; });
            for (u32 e = 0; e < ne; ++e) start[e + 1] += start[e];
            std::vector<u32> cursor(start.begin(), start.end() - 1);
            for (auto& ps : parsers) ps.each(h, rank.data(), [&](u32 e, u32, u32 l) { latency[cursor[e]++] = l; });
            finished[h] = true;
            if (!releasing.test_and_set()) {
                for (auto& ps : parsers) ps.release(finished.data());
                releasing.clear();
            }

            u64 size = 0;
            for (u32 e = 0; e < ne; ++e) {
                u32 n = start[e + 1] - start[e];
                if (n == 0) continue;
                // Nearest rank p is index ceil(p * n / 100) - 1. A selection
                // leaves only larger values after its rank, so each higher rank
                // is selected among those: half the group for p95, a twentieth
                // for p99. (An equal rank is already in place.)
                u32* a = latency.data() + start[e];
                u32 r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
                for (u32 from = 0; u32 r : {r50, r95, r99})
                    if (r >= from) select(a + from, n - from, r - from), from = r + 1;
                const Line& l = lines[h].emplace_back(e, n, a[r50], a[r95], a[r99], errors[e]);
                // Label, name, six numbers each after a space, newline.
                size += 21 + names[e].size() + 7;
                for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)}) size += width(x);
            }
            at[h] = size;
        }
        }
        sized.arrive_and_wait();

        for (int h; (h = claim_text++) < HOURS;) {
            char label[24];
            int day = (h + 18) / 24;
            snprintf(label, sizeof label, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11, day < 2 ? 30 + day : day - 1, (h + 18) % 24);
            char* o = text + at[h];
            for (const Line& l : lines[h]) {
                o = std::copy_n(label, 21, o);
                o = std::copy(names[l.e].begin(), names[l.e].end(), o);
                for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)}) {
                    *o++ = ' ';
                    o = std::to_chars(o, o + 10, x).ptr;
                }
                *o++ = '\n';
            }
            std::vector<Line>().swap(lines[h]);
        }
    });
    for (auto& th : pool) th.join();
    close(out);
    return 0;
}
