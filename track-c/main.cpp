// Speed Demon 2026, Track C (Join).
//
// Both files are memory-mapped, and every phase is cut into pieces that any
// thread of one pool claims; a phase starts once the one before is done:
//  1. regions.csv is split by lines; each thread flags the country codes it
//     sees. Countries get indices 1..N in code order (0 is _UNMATCHED).
//  2. Each thread fills its slice of a direct table region_id -> country index.
//  3. records.csv is split at CRLFs: the generator never writes a CR inside
//     quotes, so the byte after a CRLF always starts a record. Each record is
//     read field by field: the id's comma is found in one 8-byte word, a quoted
//     field's end by stepping through the bit mask of its quotes (a "" pair is
//     an escape), so commas and LFs inside quotes cost nothing, and the region's
//     comma and the LF in one 32-byte compare each. Numbers are decoded
//     eight digits at a time. Sums go to the thread's own per-country totals. Names
//     go through a shared lock-free intern table to dense ids, which are marked
//     in one bitset per country; both steps are prefetched records ahead. A
//     mark that sets a new bit adds one to the country's distinct names.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <mutex>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

// Zero-filled anonymous memory; pages are charged only once touched.
template <class T>
T* zeroed(size_t n) {
    void* p = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); _exit(1); }
    return static_cast<T*>(p);
}

// A table that every thread writes all over is populated up front: faulting
// it in page by page from twelve threads at once made the threads queue on
// the kernel's page-table locks (about 12% of all cycles), and each page was
// faulted twice, once on its first read (the shared zero page) and again on
// its first write. Thread t of n populates its share of the n pieces in one
// call, so the work is spread too. (Without MADV_POPULATE_WRITE the pages
// are simply faulted in later.)
template <class T>
void populate_share(T* p, size_t n, int t, int nthreads) {
    const size_t pages = (n * sizeof(T) + 4095) / 4096, from = pages * t / nthreads, to = pages * (t + 1) / nthreads;
    if (to > from) madvise(reinterpret_cast<char*>(p) + from * 4096, (to - from) * 4096, MADV_POPULATE_WRITE);
}

// Map a file over a spare zero page, so 64-byte loads past its end are safe.
const char* map_file(const char* path, size_t& size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); _exit(1); }
    size = lseek(fd, 0, SEEK_END);
    auto* p = static_cast<char*>(
        mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (size && mmap(p, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("mmap");
        _exit(1);
    }
    close(fd);
    return p;
}

u64 load8(const char* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

// Bit i is set when byte i of the 32 at p equals c.
u32 match32(const char* p, char c) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}


// The value of the last n (1 to 8) of the eight digit bytes in v (the first
// byte is the lowest): reduced to digit values, the bytes before them masked
// to leading zeros, then folded in pairs, quads and halves.
u64 fold(u64 v, size_t n) {
    v &= 0x0F0F0F0F0F0F0F0F & ~u64(0) << 8 * (8 - n);
    v = (v * 10 + (v >> 8)) & 0x00FF00FF00FF00FF;
    v = (v * 100 + (v >> 16)) & 0x0000FFFF0000FFFF;
    return (v * 10000 + (v >> 32)) & 0xFFFFFFFF;
}

// The value of the 1 to 8 decimal digits in [s, e).
u64 digits(const char* s, const char* e) { return fold(load8(e - 8), e - s); }

// An amount's value in cents, from its digits [s, e) with the dot third from
// the end: at most 7 digits (99999.99), so the eight bytes ending at e, with
// the dot's byte dropped, are one number.
u64 money(const char* s, const char* e) {
    u64 w = load8(e - 8);
    return fold((w & 0xFFFFFFFFFF) << 8 | (w & 0xFFFF000000000000), e - s - 1);
}

// The first c at or after p (one is known to follow). 64 bytes at a time:
// a note is often longer than 32, and the loop's exit is then predictable.
[[gnu::always_inline]] inline const char* find(const char* p, char c) {
    for (;; p += 64)
        if (u64 m = match32(p, c) | u64(match32(p + 32, c)) << 32) return p + __builtin_ctzll(m);
}

// The comma after a number of at most 7 digits starting at p: the lowest zero
// byte of the 8 bytes at p XOR ','.
const char* comma_after(const char* p) {
    u64 x = load8(p) ^ 0x2C2C2C2C2C2C2C2C;
    return p + __builtin_ctzll((x - 0x0101010101010101) & ~x & 0x8080808080808080) / 8;
}

// The text field (name or note) at p, moving p past its comma: its raw bytes
// (between the quotes, if quoted) and its value's length (one byte less per
// "" pair).
struct Text {
    const char* at;
    u32 raw, length;
};

[[gnu::always_inline]] inline Text text(const char*& p) {
    const char* s = p;
    if (*s != '"') {
        p = find(s, ',') + 1;
        return {s, u32(p - 1 - s), u32(p - 1 - s)};
    }
    // The quotes after the opening one, 64 bytes at a time, are stepped
    // through on their bit mask: a quote followed by another is a "" pair,
    // the first one that is not closes the field.
    u32 pairs = 0;
    const char* q = s + 1;
    for (u64 m = match32(q, '"') | u64(match32(q + 32, '"')) << 32;;) {
        u32 i = m ? __builtin_ctzll(m) : 64;
        if (i >= 63) {  // none, or the byte after it is out of view: move on
            q += i;
            m = match32(q, '"') | u64(match32(q + 32, '"')) << 32;
            continue;
        }
        if (!(m >> (i + 1) & 1)) {
            q += i;
            break;
        }
        ++pairs;
        m &= m - 1;
        m &= m - 1;
    }
    p = q + 2;
    return {s + 1, u32(q - s - 1), u32(q - s - 1 - pairs)};
}

// Names, interned by their raw bytes between the quotes. Escaping is one to
// one, so equal raw bytes mean equal values (and the empty name is one value
// whether quoted or not). A name's key is a 64-bit hash of those bytes and
// their length; two of the 100,000 pool names share one with a chance of
// about 3 in 10^10. A key's slot in the open-addressed table is the name's id;
// an empty slot is claimed with a compare-and-swap. At 8 bytes a slot the
// table is 1 MB, small enough to stay in a core's L2 while the input streams
// past, and eight slots share a cache line, so a probe rarely leaves it.
class NameTable {
public:
    static constexpr u32 BITS = 17, SLOTS = 1 << BITS;  // the pool holds 100,000 names

    static u64 key(const char* s, u32 len) {
        static constexpr char KEEP[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        __m128i head = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        head = _mm_and_si128(head, _mm_loadu_si128(reinterpret_cast<const __m128i*>(KEEP + 16 - std::min(len, 16u))));
        u64 h = (u64(_mm_cvtsi128_si64(head)) * 0x9E3779B97F4A7C15ull ^ len) + u64(_mm_extract_epi64(head, 1)) * 0xBF58476D1CE4E5B9ull;
        for (u32 i = 16; i < len; i += 8) {
            u64 w = load8(s + i);
            if (len - i < 8) w &= (u64(1) << 8 * (len - i)) - 1;
            h = (h ^ w) * 0x94D049BB133111EBull;
        }
        h = (h ^ h >> 31) * 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return h + (h == 0);  // 0 marks a free slot
    }

    void prefetch(u64 k) const { _mm_prefetch(reinterpret_cast<const char*>(&slots_[k >> (64 - BITS)]), _MM_HINT_T0); }

    // Slots are probed four at a time, from the aligned group of four the key
    // hashes to: one 32-byte compare finds the key, or the first free slot,
    // which is claimed (no key ever sits past a free slot, as none is freed).
    u32 id(u64 k) {
        for (u32 i = k >> (64 - BITS) & ~3u;; i = (i + 4) % SLOTS) {
            __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(&slots_[i]));
            u32 hit = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, _mm256_set1_epi64x(k))));
            u32 free = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, _mm256_setzero_si256())));
            if (hit) return i + __builtin_ctz(hit);
            for (; free; free &= free - 1) {
                u32 j = i + __builtin_ctz(free);
                u64 s = 0;
                if (slots_[j].compare_exchange_strong(s, k) || s == k) return j;
            }
        }
    }

    // Each of n threads populates its share up front (see zeroed).
    void populate(int t, int n) const { populate_share(slots_, SLOTS, t, n); }

private:
    std::atomic<u64>* slots_ = zeroed<std::atomic<u64>>(SLOTS);  // 0 = free
};

// One country's running aggregates (per thread, summed at the end). The
// struct is exactly one cache line, so aligned to one, a record's update
// touches one line instead of two.
struct alignas(64) Totals {
    u64 count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0, distinct = 0;
    i64 amount_sum = 0, amount_min = INT64_MAX, amount_max = INT64_MIN;
};

// Which names each country has: one bitset over name ids per country, shared
// by all threads. Most marks are first sightings, so they are atomic ORs.
struct NamesSeen {
    static constexpr u32 WORDS = NameTable::SLOTS / 64;
    std::atomic<u64>* bits = zeroed<std::atomic<u64>>(256 * WORDS);

    std::atomic<u64>& word(u32 c, u32 name) { return bits[c * WORDS + name / 64]; }

    // For writing: the OR needs the line exclusive, and a plain prefetch would
    // fetch it shared first.
    void prefetch(u32 c, u32 name) { __builtin_prefetch(&word(c, name), 1); }

    // Whether this is the first time the name is marked for the country, so
    // the distinct counts come out of the marking (an OR returning the old
    // bit) with no pass over the bitsets afterwards. No test first: the read
    // would fetch the line shared, then the OR would fetch it again exclusive.
    bool mark(u32 c, u32 name) {
        u64 bit = u64(1) << name % 64;
        return !(word(c, name).fetch_or(bit, std::memory_order_relaxed) & bit);
    }
};

// Parse every record that starts in [p, end). A record's region row and name
// slot are prefetched when it is parsed; LAG records later its country is
// read, its sums go to totals, its name is looked up and that name's bitset
// word prefetched; LAG records after that the name is marked. So each of the
// three random reads (region table, name table, bitsets) has a steady lead.
void parse_records(const char* p, const char* end, const char* file_end, const u8* country_of,
                   size_t regions, NameTable& names, NamesSeen& seen, Totals* totals) {
    constexpr u64 LAG = 16;
    struct Pending {
        u64 name;
        u32 region, id, name_bytes, note_bytes;
        i64 cents;
    } pending[LAG];
    struct Mark {
        u32 country, id;
    } marks[LAG];
    u64 n = 0, last = UINT64_MAX;  // records started; all of them once known
    auto advance = [&]() __attribute__((always_inline)) {  // retire ring slot n % LAG
        u32 k = n % LAG;
        if (n >= 2 * LAG && n - 2 * LAG < last) totals[marks[k].country].distinct += seen.mark(marks[k].country, marks[k].id);
        if (n >= LAG && n - LAG < last) {
            const Pending& old = pending[k];
            u32 c = old.region < regions ? country_of[old.region] : 0;
            Totals& t = totals[c];
            ++t.count;
            t.id_sum += old.id;
            t.amount_sum += old.cents;
            t.amount_min = std::min(t.amount_min, old.cents);
            t.amount_max = std::max(t.amount_max, old.cents);
            t.name_bytes += old.name_bytes;
            t.note_bytes += old.note_bytes;
            marks[k] = {c, names.id(old.name)};
            seen.prefetch(c, marks[k].id);
        }
    };

    while (p < end) {
        // The file's pages are scattered in memory, and the hardware prefetcher
        // stops at each 4 KB page; this reaches into the next one in time.
        _mm_prefetch(p + 2048, _MM_HINT_T0);
        if (u8(*p) == 0xEF) p += 3;  // a byte order mark before the id
        const char* f1 = comma_after(p);
        u64 id = digits(p, f1);
        p = f1 + 1;
        Text name = text(p), note = text(p);
        // The region (at most 7 digits) and the amount (at most 9 bytes) end
        // within the 32 bytes at p, so one load gives both the comma and the
        // LF, if any.
        u32 lf = match32(p, '\n');
        const char* f4 = p + __builtin_ctz(match32(p, ','));
        u64 region = digits(p, f4);
        const char* amount = f4 + 1;
        const char* eol = lf ? p + __builtin_ctz(lf) : file_end;
        const char* amount_end = eol - (eol[-1] == '\r');
        bool negative = *amount == '-';
        i64 cents = money(amount + negative, amount_end);
        if (negative) cents = -cents;

        // Region ids are below 10^7 (fit u32); one past the table means none.
        u32 r = region < regions ? u32(region) : u32(regions);
        _mm_prefetch(reinterpret_cast<const char*>(country_of + r), _MM_HINT_T0);
        advance();
        u64 key = NameTable::key(name.at, name.raw);
        pending[n++ % LAG] = {key, r, u32(id), name.length, note.length, cents};
        names.prefetch(key);
        p = eol + 1;
    }
    for (last = n; n < last + 2 * LAG; ++n) advance();
}

// 15-bit code of a three-letter country; numeric order is alphabetical order.
u32 code(const char* p) { return (p[0] - 'A') << 10 | (p[1] - 'A') << 5 | (p[2] - 'A'); }

// The region line "id,ABC" at p, moving p to the next line. An id has at most
// 7 digits, so the comma is the lowest zero byte of the line's first 8 bytes
// XOR ','.
std::pair<u64, u32> region_line(const char*& p) {
    u64 x = load8(p) ^ 0x2C2C2C2C2C2C2C2C;
    const char* comma = p + __builtin_ctzll((x - 0x0101010101010101) & ~x & 0x8080808080808080) / 8;
    std::pair<u64, u32> line{digits(p, comma), code(comma + 1)};
    for (p = comma + 4; *p++ != '\n';) {}
    return line;
}

// A phase of work in pieces. Every thread claims pieces until none are left,
// then sleeps until all are done. It waits for the work, not for every thread
// to arrive: a thread starts milliseconds after the one before it, and the
// late ones just join in wherever the work has got to.
struct Phase {
    std::atomic<int> claimed{0}, done{0};
    const int pieces;

    template <class F>
    void run(F&& f) {
        for (int k; (k = claimed++) < pieces;)
            if (f(k), ++done == pieces) done.notify_all();
        for (int d; (d = done) < pieces;) done.wait(d);
    }
};

// Cuts of [from, size) into n ranges, each starting just after a delimiter.
std::vector<size_t> split(const char* data, size_t from, size_t size, int n, std::string_view delim) {
    std::vector<size_t> cut(n + 1, size);
    cut[0] = from;
    for (int t = 1; t < n; ++t) {
        size_t c = std::max(cut[t - 1], from + (size - from) * t / n);
        auto* hit = static_cast<const char*>(memmem(data + c, size - c, delim.data(), delim.size()));
        cut[t] = hit ? hit - data + delim.size() : size;
    }
    return cut;
}

void put_money(std::string& out, i64 cents) {
    u64 a = cents < 0 ? -u64(cents) : u64(cents);
    out += cents < 0 ? " -" : " ";
    out += std::to_string(a / 100);
    out += {'.', char('0' + a % 100 / 10), char('0' + a % 10)};
}

int main() {
    // With transparent huge pages, a block written only in part can be backed
    // by a whole 2 MB page; plain pages keep memory to what is written.
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    const int nthreads = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
    size_t rsize, size;
    const char* regions = map_file("/data/regions.csv", rsize);
    const char* records = map_file("/data/records.csv", size);

    // regions.csv: a header, then "id,ABC\n" lines in ascending id order, so
    // the last line holds the largest id.
    const char* last = static_cast<const char*>(memrchr(regions, '\n', rsize - 1)) + 1;
    const size_t nregions = atoll(last) + 1;
    u8* country_of = zeroed<u8>(nregions);  // 0 = no such region
    std::vector<size_t> rcut = split(regions, strchr(regions, '\n') - regions + 1, rsize, nthreads, "\n");
    std::vector<std::atomic<u8>> present(1 << 15);
    std::array<u8, 1 << 15> index_of{};
    std::vector<u32> codes;  // country index - 1 -> code
    std::once_flag numbered;
    auto number_countries = [&] {
        for (u32 k = 0; k < present.size(); ++k)
            if (present[k]) codes.push_back(k), index_of[k] = codes.size();
        if (codes.size() > 255) { fputs("too many countries\n", stderr); _exit(1); }
    };

    // records.csv: skip the BOM and header, then split after CRLFs into eight
    // pieces per thread, claimed in turn, so a thread whose pieces are slow
    // (more first sightings of names) is evened out by the others.
    size_t start = size ? static_cast<const char*>(memchr(records, '\n', size)) - records + 1 : 0;
    const int pieces = 8 * nthreads;
    std::vector<size_t> cut = split(records, start, size, pieces, "\r\n");
    std::vector<std::array<Totals, 256>> totals(nthreads);
    NameTable names;
    NamesSeen seen;

    // Populate tables and flag countries; populate bitsets and fill the region
    // table; parse records.
    Phase flag{.pieces = nthreads}, fill{.pieces = nthreads}, parse{.pieces = pieces};
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t)
        pool.emplace_back([&, t] {
            flag.run([&](int k) {
                populate_share(country_of, nregions, k, nthreads);
                names.populate(k, nthreads);
                for (const char* p = regions + rcut[k]; p < regions + rcut[k + 1];)
                    present[region_line(p).second].store(1, std::memory_order_relaxed);
            });
            std::call_once(numbered, number_countries);
            fill.run([&](int k) {
                populate_share(seen.bits, (codes.size() + 1) * NamesSeen::WORDS, k, nthreads);
                for (const char* p = regions + rcut[k]; p < regions + rcut[k + 1];) {
                    auto [id, c] = region_line(p);
                    country_of[id] = index_of[c];
                }
            });
            parse.run([&](int k) {
                // Map the piece's pages in one call rather than by a fault
                // every 16 pages as the parse reaches them.
                uintptr_t from = uintptr_t(records + cut[k]) & ~uintptr_t(4095);
                madvise(reinterpret_cast<void*>(from), uintptr_t(records + cut[k + 1]) - from, MADV_POPULATE_READ);
                parse_records(records + cut[k], records + cut[k + 1], records + size, country_of, nregions, names,
                              seen, totals[t].data());
                // The piece is not read again: its pages are unmapped now, by
                // this thread, instead of all of the file's at exit by one.
                uintptr_t to = uintptr_t(records + cut[k + 1]) & ~uintptr_t(4095);
                if (to > from) madvise(reinterpret_cast<void*>(from), to - from, MADV_DONTNEED);
            });
        });
    for (auto& th : pool) th.join();

    std::string out;
    for (u32 i = 1; i <= codes.size() + 1; ++i) {
        u32 c = i % (codes.size() + 1);  // countries 1..N, then _UNMATCHED (0)
        Totals sum;
        for (const auto& per_thread : totals) {
            const Totals& t = per_thread[c];
            sum.count += t.count;
            sum.id_sum += t.id_sum;
            sum.name_bytes += t.name_bytes;
            sum.note_bytes += t.note_bytes;
            sum.distinct += t.distinct;
            sum.amount_sum += t.amount_sum;
            sum.amount_min = std::min(sum.amount_min, t.amount_min);
            sum.amount_max = std::max(sum.amount_max, t.amount_max);
        }
        if (sum.count == 0) continue;
        if (c == 0) {
            out += "_UNMATCHED";
        } else {
            u32 k = codes[c - 1];
            out += {char('A' + (k >> 10)), char('A' + (k >> 5 & 31)), char('A' + (k & 31))};
        }
        for (u64 v : {sum.count, sum.id_sum}) out += ' ' + std::to_string(v);
        for (i64 v : {sum.amount_sum, sum.amount_min, sum.amount_max}) put_money(out, v);
        for (u64 v : {sum.distinct, sum.name_bytes, sum.note_bytes}) out += ' ' + std::to_string(v);
        out += '\n';
    }
    FILE* f = fopen("/out/aggregates.txt", "wb");
    if (!f) { perror("/out/aggregates.txt"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
