// Speed Demon 2026, Track C (Join).
//
// Both files are memory-mapped and every phase runs on one pool of threads,
// separated by barriers:
//  1. regions.csv is split by lines; each thread flags the country codes it
//     sees. Countries get indices 1..N in code order (0 is _UNMATCHED).
//  2. Each thread fills its slice of a direct table region_id -> country index.
//  3. records.csv is split at CRLFs: the generator never writes a CR inside
//     quotes, so the byte after a CRLF always starts a record. Each thread
//     scans 64-byte blocks for quotes, commas and LFs, masks out the ones inside
//     quotes with a carry-less prefix XOR, and reads each record's four commas
//     and terminator straight off the bit mask. Numbers are decoded eight
//     digits at a time. Sums go to the thread's own per-country totals. Names
//     go through a shared lock-free intern table to dense ids, which are marked
//     in one bitset per country; both steps are prefetched records ahead.
//  4. Each thread counts the set bits of every nthreads-th country.
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
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

u64 match64(const char* p, char c) { return match32(p, c) | u64(match32(p + 32, c)) << 32; }

// The value of the 1 to 8 decimal digits in [s, e): the eight bytes ending at
// e, reduced to digit values, with the bytes before s masked to leading zeros
// (the first byte is the lowest), are folded in pairs, quads and halves.
u64 digits(const char* s, const char* e) {
    u64 v = load8(e - 8) & 0x0F0F0F0F0F0F0F0F & ~u64(0) << 8 * (8 - (e - s));
    v = (v * 10 + (v >> 8)) & 0x00FF00FF00FF00FF;
    v = (v * 100 + (v >> 16)) & 0x0000FFFF0000FFFF;
    return (v * 10000 + (v >> 32)) & 0xFFFFFFFF;
}

// The commas and LFs outside quotes, in order, from a record start onwards,
// each with the number of '"' bytes before it (so a field's quotes are a
// difference, with no second pass over its bytes).
class Structure {
public:
    struct Separator {
        const char* at;
        u64 quotes;
    };

    Structure(const char* p, const char* file_end) : block_(p - 64), end_(file_end) {}

    // The next separator, or the end of the file once there is none.
    Separator next() {
        while (bits_ == 0) {
            block_ += 64;
            if (block_ >= end_) return {end_, before_ + __builtin_popcountll(q_)};
            before_ += __builtin_popcountll(q_);
            q_ = match64(block_, '"');
            // Bit i of the prefix XOR is set between an opening quote and its
            // closing quote; a "" pair flips it twice and so changes nothing.
            __m128i x = _mm_clmulepi64_si128(_mm_cvtsi64_si128(q_), _mm_set1_epi8(-1), 0);
            u64 inside = u64(_mm_cvtsi128_si64(x)) ^ carry_;
            carry_ = u64(i64(inside) >> 63);
            bits_ = (match64(block_, ',') | match64(block_, '\n')) & ~inside;
        }
        u32 i = __builtin_ctzll(bits_);
        bits_ &= bits_ - 1;
        return {block_ + i, before_ + __builtin_popcountll(q_ & ((u64(1) << i) - 1))};
    }

private:
    const char* block_;
    const char* end_;
    u64 bits_ = 0, carry_ = 0, q_ = 0, before_ = 0;  // q_: quote bits of the block
};

// Names, interned by their raw bytes between the quotes. Escaping is one to
// one, so equal raw bytes mean equal values (and the empty name is one value
// whether quoted or not). A name's 16-byte key is its bytes, zero padded,
// which is exact for names of up to 16 bytes (no name holds a zero byte). A
// longer name keeps its first 8 bytes and a 63-bit hash of the rest, flagged
// by the top bit, which no ASCII byte sets: two names would have to share
// their first 8 bytes and that hash to be confused. The empty name is the key
// {0, top bit}. A key's slot in the open-addressed table is the name's id. A
// slot is claimed with a 16-byte compare-and-swap and read with one aligned
// 16-byte load, so a reader always sees it empty or whole.
class NameTable {
public:
    static constexpr u32 BITS = 17, SLOTS = 1 << BITS;  // the pool holds 100,000 names
    static constexpr u64 FLAG = u64(1) << 63;

    static __m128i key(const char* s, u32 len) {
        static constexpr char KEEP[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        __m128i head = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        head = _mm_and_si128(head, _mm_loadu_si128(reinterpret_cast<const __m128i*>(KEEP + 16 - std::min(len, 16u))));
        if (len == 0) return _mm_set_epi64x(FLAG, 0);
        if (len <= 16) return head;
        u64 h = len;
        for (u32 i = 8; i < len; i += 8) {
            u64 w = load8(s + i);
            if (len - i < 8) w &= (u64(1) << 8 * (len - i)) - 1;
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 32;
        }
        return _mm_set_epi64x(h | FLAG, _mm_cvtsi128_si64(head));
    }

    static u32 home(__m128i k) {
        u64 h = u64(_mm_cvtsi128_si64(k)) * 0x9E3779B97F4A7C15ull ^ u64(_mm_extract_epi64(k, 1)) * 0xBF58476D1CE4E5B9ull;
        return h >> (64 - BITS);
    }

    void prefetch(__m128i k) const { _mm_prefetch(reinterpret_cast<const char*>(&slots_[home(k)]), _MM_HINT_T0); }

    u32 id(__m128i k) {
        for (u32 i = home(k);; i = (i + 1) % SLOTS) {
            __m128i s = _mm_load_si128(&slots_[i]);
            if (_mm_testz_si128(s, s)) {
                unsigned __int128 want;
                memcpy(&want, &k, 16);
                if (__sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128*>(&slots_[i]), 0, want)) return i;
                s = _mm_load_si128(&slots_[i]);  // another thread took it first
            }
            if (_mm_movemask_epi8(_mm_cmpeq_epi8(s, k)) == 0xFFFF) return i;
        }
    }

    // Each of n threads populates its share up front (see zeroed).
    void populate(int t, int n) const { populate_share(slots_, SLOTS, t, n); }

private:
    __m128i* slots_ = zeroed<__m128i>(SLOTS);  // all zero = free
};

// One country's running aggregates (per thread, summed at the end).
struct Totals {
    u64 count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0, distinct = 0;
    i64 amount_sum = 0, amount_min = INT64_MAX, amount_max = INT64_MIN;
};

// Which names each country has: one bitset over name ids per country, shared
// by all threads. Most marks are first sightings, so they are atomic ORs.
struct NamesSeen {
    static constexpr u32 WORDS = NameTable::SLOTS / 64;
    std::atomic<u64>* bits = zeroed<std::atomic<u64>>(256 * WORDS);

    std::atomic<u64>& word(u32 c, u32 name) { return bits[c * WORDS + name / 64]; }

    void prefetch(u32 c, u32 name) { _mm_prefetch(reinterpret_cast<const char*>(&word(c, name)), _MM_HINT_T0); }

    void mark(u32 c, u32 name) {
        u64 bit = u64(1) << name % 64;
        if (!(word(c, name).load(std::memory_order_relaxed) & bit))
            word(c, name).fetch_or(bit, std::memory_order_relaxed);
    }

    u64 count(u32 c) const {
        u64 n = 0;
        for (u32 w = 0; w < WORDS; ++w) n += __builtin_popcountll(bits[c * WORDS + w].load(std::memory_order_relaxed));
        return n;
    }
};

// A text field's value length: its bytes, less the delimiting quotes and one
// byte per "" pair when it is quoted (q counts every quote in it).
u32 text_length(const char* s, const char* e, u32 q) { return (e - s) - (q ? q / 2 + 1 : 0); }

// Parse every record that starts in [p, end). A record's region row and name
// slot are prefetched when it is parsed; LAG records later its country is
// read, its sums go to totals, its name is looked up and that name's bitset
// word prefetched; LAG records after that the name is marked. So each of the
// three random reads (region table, name table, bitsets) has a steady lead.
void parse_records(const char* p, const char* end, const char* file_end, const u8* country_of,
                   size_t regions, NameTable& names, NamesSeen& seen, Totals* totals) {
    constexpr u64 LAG = 16;
    struct Pending {
        __m128i name;
        u32 region, id, name_bytes, note_bytes;
        i64 cents;
    } pending[LAG];
    struct Mark {
        u32 country, id;
    } marks[LAG];
    u64 n = 0, last = UINT64_MAX;  // records started; all of them once known
    auto advance = [&]() __attribute__((always_inline)) {  // retire ring slot n % LAG
        u32 k = n % LAG;
        if (n >= 2 * LAG && n - 2 * LAG < last) seen.mark(marks[k].country, marks[k].id);
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

    Structure sep(p, file_end);
    while (p < end) {
        auto [f1, q1] = sep.next();  // the commas after id, name, note, region
        auto [f2, q2] = sep.next();
        auto [f3, q3] = sep.next();
        const char* f4 = sep.next().at;
        const char* eol = sep.next().at;  // LF, or the end of the file
        if (u8(*p) == 0xEF) p += 3;       // a byte order mark before the id
        u64 id = digits(p, f1);
        u64 region = digits(f3 + 1, f4);
        const char* amount = f4 + 1;
        const char* amount_end = eol - (eol[-1] == '\r');
        bool negative = *amount == '-';
        i64 cents = digits(amount + negative, amount_end - 3) * 100 + digits(amount_end - 2, amount_end);
        if (negative) cents = -cents;

        // Region ids are below 10^7 (fit u32); one past the table means none.
        u32 r = region < regions ? u32(region) : u32(regions);
        _mm_prefetch(reinterpret_cast<const char*>(country_of + r), _MM_HINT_T0);
        u32 name_quotes = q2 - q1;
        bool quoted = name_quotes != 0;
        advance();
        Pending& next = pending[n++ % LAG];
        next = {NameTable::key(f1 + 1 + quoted, u32(f2 - f1 - 1 - 2 * quoted)), r, u32(id),
                text_length(f1 + 1, f2, name_quotes), text_length(f2 + 1, f3, q3 - q2), cents};
        names.prefetch(next.name);
        p = eol + 1;
    }
    for (last = n; n < last + 2 * LAG; ++n) advance();
}

// 15-bit code of a three-letter country; numeric order is alphabetical order.
u32 code(const char* p) { return (p[0] - 'A') << 10 | (p[1] - 'A') << 5 | (p[2] - 'A'); }

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
    auto number_countries = [&]() noexcept {  // runs at every barrier; works once
        if (!codes.empty()) return;
        for (u32 k = 0; k < present.size(); ++k)
            if (present[k]) codes.push_back(k), index_of[k] = codes.size();
        if (codes.size() > 255) { fputs("too many countries\n", stderr); _exit(1); }
    };
    std::barrier sync(nthreads, number_countries);

    // records.csv: skip the BOM and header, then split after CRLFs into four
    // pieces per thread, claimed in turn, so a thread whose pieces are slow
    // (more first sightings of names) is evened out by the others.
    size_t start = size ? static_cast<const char*>(memchr(records, '\n', size)) - records + 1 : 0;
    const int pieces = 4 * nthreads;
    std::vector<size_t> cut = split(records, start, size, pieces, "\r\n");
    std::atomic<int> claim{0};
    std::vector<std::array<Totals, 256>> totals(nthreads);
    NameTable names;
    NamesSeen seen;

    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t)
        pool.emplace_back([&, t] {
            const char* from = regions + rcut[t];
            const char* to = regions + rcut[t + 1];
            populate_share(country_of, nregions, t, nthreads);
            names.populate(t, nthreads);
            for (const char* p = from; p < to; p = strchr(p, '\n') + 1)
                present[code(strchr(p, ',') + 1)].store(1, std::memory_order_relaxed);
            sync.arrive_and_wait();
            populate_share(seen.bits, (codes.size() + 1) * NamesSeen::WORDS, t, nthreads);
            for (const char* p = from; p < to; p = strchr(p, '\n') + 1) {
                const char* comma = strchr(p, ',');
                country_of[digits(p, comma)] = index_of[code(comma + 1)];
            }
            sync.arrive_and_wait();
            for (int k; (k = claim++) < pieces;)
                parse_records(records + cut[k], records + cut[k + 1], records + size, country_of,
                              nregions, names, seen, totals[t].data());
            sync.arrive_and_wait();
            for (u32 c = t; c <= codes.size(); c += nthreads) totals[t][c].distinct = seen.count(c);
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
