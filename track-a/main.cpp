// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads does everything in two phases.
//
// Phase 1, parse. The log is memory-mapped and cut into line-aligned ranges,
// one per thread. The timestamp is always 27 bytes and the 72-hour span is
// fixed, so a line's hour index comes from its day and hour digits alone. One
// 64-byte scan after the timestamp finds the method's end, the endpoint's end
// and the line's end. Each thread interns endpoints in its own small table and
// files a 4-byte record (latency, local endpoint id, 5xx flag) under the hour.
//
// Between the phases the last thread to arrive merges the endpoint tables and
// sorts the names, so an endpoint's global id is its rank in output order.
//
// Phase 2, hours. Threads claim hours in order. An hour's ~43k latencies are
// counting-sorted by endpoint rank into per-thread scratch, three selections
// per group give the percentiles, and the lines are formatted into a reusable
// per-thread buffer. Each hour learns its file offset from the hour before and
// writes itself with pwrite, so no output text is ever held for later.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;

constexpr int HOURS = 72;  // 2026-10-30T18:00Z to 2026-11-02T17:59:59Z
// A record packs the latency above a 12-bit key: an 11-bit local endpoint id
// and the 5xx flag. Latencies of 2^20 us or more (about 1% of lines) do not
// fit and are kept on a side list with their key instead.
constexpr u32 KEY_BITS = 12, ERR = 1u << 11, ID_MASK = ERR - 1, PACKED_MAX = 1u << 20;

// Bit i is set when byte i of the 32 at p equals c.
inline u32 eq32(const char* p, char c) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}

// The first c at or after s; one is known to follow.
inline const char* next_byte(const char* s, char c) {
    for (;; s += 32)
        if (u32 m = eq32(s, c)) return s + __builtin_ctz(m);
}

// The value of the len (1 to 10) digits ending just before end, with no loop:
// the 16 bytes before end, minus '0' and with the bytes ahead of the number
// zeroed, are combined 2, 4 and then 8 digits at a time.
inline u32 decode(const char* end, u32 len) {
    __m128i v = _mm_subs_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 16)), _mm_set1_epi8('0'));
    __m128i lane = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
    v = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));  // pairs (10, 1)
    v = _mm_madd_epi16(v, _mm_set1_epi32(0x00010064));  // pairs (100, 1)
    v = _mm_packus_epi32(v, v);
    v = _mm_madd_epi16(v, _mm_set1_epi32(0x00012710));  // pairs (10000, 1)
    return u32(u64(u32(_mm_cvtsi128_si32(v))) * 100000000 + u32(_mm_extract_epi32(v, 1)));
}

// Endpoint names to dense ids in first-seen order. Open addressing on a CRC of
// the name's first and last eight bytes (masked when the name is shorter; the
// rest of the line keeps both loads in bounds). Names point into the log.
struct Interner {
    static constexpr u32 SLOTS = 1 << 12;  // ids fit 11 bits; a seed has 1,842
    std::vector<std::string_view> names;
    std::vector<uint16_t> slot = std::vector<uint16_t>(SLOTS);  // id + 1, 0 if free

    u32 operator()(std::string_view s) {
        u64 a, b = 0;
        memcpy(&a, s.data(), 8);
        if (s.size() >= 8) memcpy(&b, s.data() + s.size() - 8, 8);
        else a &= (u64(1) << 8 * s.size()) - 1;
        for (u32 i = _mm_crc32_u64(_mm_crc32_u64(s.size(), a), b);; ++i) {
            uint16_t& id = slot[i % SLOTS];
            if (id == 0) {
                if (names.size() == SLOTS / 2) { fputs("too many endpoints\n", stderr); _exit(1); }
                names.push_back(s);
                id = names.size();
            }
            if (names[id - 1] == s) return id - 1;
        }
    }
};

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
    Interner local;
    Chunk* last[HOURS] = {};
    std::vector<std::pair<u32, u32>> wide[HOURS];  // (latency, key) of the long ones
    std::vector<u32> rank;                        // local id -> global rank
    Chunk *region = nullptr, *next = nullptr, *kept = nullptr;  // kept: first chunk not yet released

    void run(const char* p, const char* end) {
        // A line is at least 40 bytes; each hour may leave one chunk part full.
        size_t bytes = ((end - p) / 40 / Chunk::CAP + 1 + HOURS) * sizeof(Chunk);
        region = next = kept = static_cast<Chunk*>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
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
            u32 key = local(std::string_view(name, name_end - name)) | (name_end[1] == '5' ? ERR : 0);
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
    // hours are all finished grows steadily. Hand it back once it reaches 256
    // KB, in one madvise, instead of one call (and TLB shootdown) per chunk.
    void release(const std::atomic<bool>* finished) {
        Chunk* c = kept;
        while (c < next && finished[c->hour]) ++c;
        if (c - kept >= 16 || (c == next && c > kept)) {
            madvise(kept, (c - kept) * sizeof(Chunk), MADV_DONTNEED);
            kept = c;
        }
    }

    // Calls f(global rank, key, latency) for every record of hour h.
    template <class F>
    void each(int h, F&& f) const {
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
    int out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) { perror("open"); return 1; }

    // Map the log over one spare zero page, so the 64-byte scans stay in bounds.
    size_t size = lseek(in, 0, SEEK_END);
    auto* log = static_cast<char*>(mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (size && mmap(log, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, in, 0) == MAP_FAILED) { perror("mmap"); return 1; }
    std::vector<size_t> cut(nt + 1, size);
    cut[0] = 0;
    for (int t = 1; t < nt; ++t)
        for (cut[t] = std::max(cut[t - 1], size * t / nt); cut[t] < size && log[cut[t] - 1] != '\n';) ++cut[t];

    // Merge step: global ids are ranks in unsigned byte order (a proper prefix
    // first), which is the order lines are printed in within an hour.
    std::vector<Parser> parsers(nt);
    std::vector<std::string_view> names;
    std::barrier merged(nt, [&]() noexcept {
        Interner all;
        for (auto& ps : parsers)
            for (auto n : ps.local.names) ps.rank.push_back(all(n));
        names = all.names;
        std::sort(names.begin(), names.end());
        std::vector<u32> rank_of(names.size());
        for (u32 r = 0; r < names.size(); ++r) rank_of[all(names[r])] = r;
        for (auto& ps : parsers)
            for (u32& id : ps.rank) id = rank_of[id];
    });

    // Hour h's first byte in the file, published once hour h - 1 is formatted.
    std::vector<std::atomic<u64>> offset(HOURS + 1);
    std::vector<std::atomic<bool>> known(HOURS + 1);
    known[0] = true;
    // An hour is finished once its records are copied out; one thread at a
    // time then releases whatever chunks that frees.
    std::vector<std::atomic<bool>> finished(HOURS);
    std::atomic_flag releasing;
    std::atomic<int> claim{0};

    std::vector<std::thread> pool;
    for (int t = 0; t < nt; ++t) pool.emplace_back([&, t] {
        parsers[t].run(log + cut[t], log + cut[t + 1]);
        merged.arrive_and_wait();

        const u32 ne = names.size();
        std::vector<u32> start(ne + 1), errors(ne), latency;
        // A line is at most 21 + 125 + 6 * 11 bytes. Left uninitialised, so
        // only the pages an hour actually writes count.
        std::unique_ptr<char[]> text(new char[size_t(ne) * 224]);
        for (int h; (h = claim++) < HOURS;) {
            // Counting sort of the hour's latencies by endpoint rank: count into
            // start[e + 1], prefix, then place each latency at its group's cursor.
            std::fill(start.begin(), start.end(), 0);
            std::fill(errors.begin(), errors.end(), 0);
            for (auto& ps : parsers) ps.each(h, [&](u32 e, u32 err, u32) { ++start[e + 1], errors[e] += err != 0; });
            for (u32 e = 0; e < ne; ++e) start[e + 1] += start[e];
            latency.resize(start[ne]);
            std::vector<u32> cursor(start.begin(), start.end() - 1);
            for (auto& ps : parsers) ps.each(h, [&](u32 e, u32, u32 l) { latency[cursor[e]++] = l; });
            finished[h] = true;
            if (!releasing.test_and_set()) {
                for (auto& ps : parsers) ps.release(finished.data());
                releasing.clear();
            }

            char label[24];
            int day = (h + 18) / 24;
            snprintf(label, sizeof label, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11, day < 2 ? 30 + day : day - 1, (h + 18) % 24);
            char* o = text.get();
            for (u32 e = 0; e < ne; ++e) {
                u32 n = start[e + 1] - start[e];
                if (n == 0) continue;
                // Nearest rank p is index ceil(p * n / 100) - 1. Selecting the
                // highest rank first leaves everything below it in front, so
                // each later selection only needs the front part.
                u32* a = latency.data() + start[e];
                u32 r99 = (99 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r50 = (50 * n + 99) / 100 - 1;
                if (n <= 64) {
                    std::sort(a, a + n);
                } else {
                    std::nth_element(a, a + r99, a + n);
                    std::nth_element(a, a + r95, a + r99);
                    std::nth_element(a, a + r50, a + r95);
                }
                o = std::copy_n(label, 21, o);
                o = std::copy(names[e].begin(), names[e].end(), o);
                for (u32 x : {n, a[r50], a[r95], a[r99], errors[e], u32(u64(errors[e]) * 1000000 / n)}) {
                    *o++ = ' ';
                    o = std::to_chars(o, o + 10, x).ptr;
                }
                *o++ = '\n';
            }

            // Take this hour's offset from the one before, pass the next one on,
            // then write; the write never waits for another thread.
            known[h].wait(false);
            u64 at = offset[h], len = o - text.get();
            offset[h + 1] = at + len;
            known[h + 1] = true;
            known[h + 1].notify_all();
            for (u64 done = 0; done < len;) {
                ssize_t w = pwrite(out, text.get() + done, len - done, at + done);
                if (w <= 0) { perror("pwrite"); _exit(1); }
                done += w;
            }
        }
    });
    for (auto& th : pool) th.join();
    close(out);
    return 0;
}
