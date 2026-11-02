// Speed Demon 2026, Track C (Join).
//
// Both files are memory-mapped, and the bitsets sit on 2 MB pages. Every
// phase is cut into pieces that any thread of one pool claims; a
// phase starts once the one before is done:
//  1. regions.csv is split by lines; each thread flags the country codes it
//     sees. Countries get indices 1..N in code order (0 is _UNMATCHED).
//  2. Each thread fills its slice of a direct table region_id -> country index.
//  3. records.csv is split at CRLFs: the generator never writes a CR inside
//     quotes, so the byte after a CRLF always starts a record. The file is
//     classified 64 bytes at a time into masks of quotes and of the commas and
//     LFs outside quotes, and the separators listed a chunk at a time; each
//     record then takes five of them. A text field's length comes from its
//     quote count, and the id, region and amount are decoded together in one
//     vector. Sums go to the thread's own per-country totals. Names go through
//     a shared lock-free intern table to ids, which are marked in one bitset
//     per country; both steps are prefetched records ahead. A mark that sets a
//     new bit adds one to the country's distinct names.
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
#include <new>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

// A fatal error: what failed (and errno's reason), then exit status 1.
[[noreturn]] void die(const char* what) {
    perror(what);
    _exit(1);
}

// Zero-filled anonymous memory; pages are charged only once touched. A huge
// block sits on 2 MB pages (whole ones, aligned); the rest refuse them, as
// the box runs huge pages always on.
constexpr size_t HUGE_PAGE = 2 << 20;
template <class T>
T* zeroed(size_t n, bool huge = false) {
    const size_t bytes = huge ? (n * sizeof(T) + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1) : n * sizeof(T);
    auto* p = static_cast<char*>(mmap(nullptr, bytes + huge * HUGE_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (p == MAP_FAILED) die("mmap");
    if (huge) p = reinterpret_cast<char*>((uintptr_t(p) + HUGE_PAGE - 1) & ~(HUGE_PAGE - 1));
    madvise(p, bytes, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    return reinterpret_cast<T*>(p);
}

// A table that every thread writes all over is populated up front: faulting
// it in page by page from twelve threads at once made the threads queue on
// the kernel's page-table locks, and each page was faulted twice, once on its
// first read (the shared zero page) and again on its first write. Thread t of
// n populates its share of the pages in one call. On 2 MB pages each page is
// one thread's: threads faulting one at once would each get a page allocated,
// all but one then freed.
template <class T>
void populate_share(T* p, size_t n, int t, int nthreads, size_t page = 4096) {
    const size_t pages = (n * sizeof(T) + page - 1) / page, from = pages * t / nthreads, to = pages * (t + 1) / nthreads;
    if (to > from) madvise(reinterpret_cast<char*>(p) + from * page, (to - from) * page, MADV_POPULATE_WRITE);
}

// Map a file over a spare zero page, so 64-byte loads past its end are safe.
const char* map_file(const char* path, size_t& size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die(path);
    size = lseek(fd, 0, SEEK_END);
    auto* p = static_cast<char*>(
        mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (size && mmap(p, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) die("mmap");
    close(fd);
    return p;
}

u64 load8(const char* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
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

// Names, interned by their raw bytes between the quotes. Escaping is one to
// one, so equal raw bytes mean equal values (and the empty name is one value
// whether quoted or not). A name's key is a 64-bit hash of those bytes and
// their length. A key's slot in the open-addressed table is the name's id; an
// empty slot is claimed with a compare-and-swap. A slot keeps 32 bits of the
// key and the slot's group 14 more, so two names are confused only if they
// share those 32 bits and meet in one group: with 100,000 names and about ten
// keys per group probed, a chance of about 1 in 10,000 per dataset. At 4 bytes
// a slot the table is 512 KB, and eight slots share a cache line.
class NameTable {
public:
    static constexpr u32 BITS = 17, SLOTS = 1 << BITS;  // the pool holds 100,000 names

    static u64 mix(u64 a, u64 b) {
        unsigned __int128 p = (unsigned __int128)a * b;
        return u64(p) ^ u64(p >> 64);
    }

    static u64 key(const char* s, u32 len) {
        static constexpr char KEEP[32] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        __m128i head = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        head = _mm_and_si128(head, _mm_loadu_si128(reinterpret_cast<const __m128i*>(KEEP + 16 - std::min(len, 16u))));
        // The two words, each XORed with a constant that has bytes of 0x80
        // and over (so never zero: the text is ASCII), multiplied to 128
        // bits and folded, as in wyhash; a longer name's further words are
        // folded in the same way.
        u64 h = mix(u64(_mm_cvtsi128_si64(head)) ^ 0x9E3779B97F4A7C15ull,
                    u64(_mm_extract_epi64(head, 1)) ^ len ^ 0xBF58476D1CE4E5B9ull);
        for (u32 i = 16; i < len; i += 8) {
            u64 w = load8(s + i);
            if (len - i < 8) w &= (u64(1) << 8 * (len - i)) - 1;
            h = mix(h ^ w, 0x94D049BB133111EBull);
        }
        return h;
    }

    void prefetch(u64 k) const { _mm_prefetch(reinterpret_cast<const char*>(&slots_[k >> (64 - BITS)]), _MM_HINT_T0); }

    // A slot holds the key's low 32 bits (never 0: 0 is a free slot); the
    // top bits choose the aligned group of eight it starts in. Groups are
    // probed whole: one 32-byte compare finds the key, or the first free
    // slot, which is claimed (no key ever sits past a free slot, as none is
    // freed).
    u32 id(u64 k) {
        const u32 tag = u32(k) | (u32(k) == 0);
        for (u32 i = k >> (64 - BITS) & ~7u;; i = (i + 8) % SLOTS) {
            __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(&slots_[i]));
            u32 hit = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(v, _mm256_set1_epi32(tag))));
            u32 free = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(v, _mm256_setzero_si256())));
            if (hit) return i + __builtin_ctz(hit);
            for (; free; free &= free - 1) {
                u32 j = i + __builtin_ctz(free), s = 0;
                if (slots_[j].compare_exchange_strong(s, tag) || s == tag) return j;
            }
        }
    }

    // Each of n threads populates its share up front (see populate_share).
    void populate(int t, int n) const { populate_share(slots_, SLOTS, t, n); }

private:
    std::atomic<u32>* slots_ = zeroed<std::atomic<u32>>(SLOTS);  // 0 = free
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
    std::atomic<u64>* bits = nullptr;  // a row per country, set once the countries are known

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
        // Probe: a plain read-modify-write, no lock (racy: can lose a bit).
        u64& w = reinterpret_cast<u64&>(word(c, name));
        u64 old = w;
        w = old | bit;
        return !(old & bit);
    }
};

// The id, region and amount of a record at once, from the eight bytes ending
// at each: one 64-bit lane each, the bytes before the number (the low ones)
// masked off so they read as leading zeros, then digit pairs, quads and
// halves folded with multiply-adds. The amount's dot is dropped first: at
// most 7 digits (99999.99), so its eight bytes less the dot are one number.
struct Numbers {
    u32 id, region, cents;
};

[[gnu::always_inline]] inline Numbers numbers(const char* id_at, const char* id_end, const char* region_at,
                                              const char* region_end, const char* amount_at, const char* amount_end) {
    u64 w = load8(amount_end - 8);
    u64 amount = (w & 0xFFFFFFFFFF) << 8 | (w & 0xFFFF000000000000);
    __m256i v = _mm256_set_epi64x(0, amount, load8(region_end - 8), load8(id_end - 8));
    __m256i shift = _mm256_set_epi64x(64, 8 * (9 - (amount_end - amount_at)), 8 * (8 - (region_end - region_at)),
                                      8 * (8 - (id_end - id_at)));
    v = _mm256_and_si256(v, _mm256_sllv_epi64(_mm256_set1_epi8(0x0F), shift));
    v = _mm256_maddubs_epi16(v, _mm256_set1_epi16(0x010A));        // 10 * first + second
    v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x00010064));        // 100 * first + second
    v = _mm256_packus_epi32(v, v);                                  // the quads as 16-bit words
    v = _mm256_madd_epi16(v, _mm256_set1_epi32(0x00012710));        // 10000 * first + second
    __m128i lo = _mm256_castsi256_si128(v);
    return {u32(_mm_cvtsi128_si32(lo)), u32(_mm_extract_epi32(lo, 1)),
            u32(_mm_cvtsi128_si32(_mm256_extracti128_si256(v, 1)))};
}

// Bit i is set when byte i of the 64 at p equals c.
[[gnu::always_inline]] inline u64 mask64(__m256i a, __m256i b, char c) {
    __m256i v = _mm256_set1_epi8(c);
    return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, v))) |
           u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, v)))) << 32;
}

// Parse every record that starts in [p, end). A record's region row and name
// slot are prefetched when it is parsed; LAG records later its country is
// read, its sums go to totals, its name is looked up and that name's bitset
// word prefetched; LAG records after that the name is marked. So each of the
// three random reads (region table, name table, bitsets) has a steady lead.
void parse_records(const char* p, const char* end, const char* file_end, const u8* country_of,
                   size_t regions, NameTable names, NamesSeen seen, Totals* totals) {
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

    // The file is read a chunk of 64-byte blocks at a time, each chunk from
    // a record's start. A block is classified at once: a bit mask each of
    // quotes, commas and LFs; the bytes inside quotes are those after an odd
    // number of quotes, the prefix XOR of the quote mask (a carry-less
    // multiply by all ones), carried from block to block (a "" pair turns
    // quoting off and on again, so it needs no care). The separators, the
    // commas and LFs outside quotes, go to an array in order, eight written
    // per block whether there are that many or not; then the chunk's whole
    // records are read from it, five separators each, and the next chunk
    // starts at the first record not whole. A field's value is its bytes
    // less one per quote pair, less the outer quotes of a quoted one: it has
    // 2 + 2k quotes for k escapes, or none.
    constexpr u32 CHUNK = 64;
    u32 at[CHUNK * 64 + 9];  // separator offsets from the chunk's start
    u64 qmask[CHUNK];
    u32 qbefore[CHUNK];
    while (p < end) {
        const u32 blocks = std::min<u64>(CHUNK, (end - p + 63) / 64);
        u32 seps = 0, quotes = 0;
        u64 inside = 0;
        for (u32 b = 0; b < blocks; ++b) {
            const char* block = p + 64 * b;
            _mm_prefetch(block + 2048, _MM_HINT_T0);
            __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block));
            __m256i hi = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block + 32));
            u64 q = mask64(lo, hi, '"');
            u64 in = _mm_cvtsi128_si64(_mm_clmulepi64_si128(_mm_cvtsi64_si128(q), _mm_set1_epi8(-1), 0)) ^ inside;
            inside = u64(i64(in) >> 63);
            u64 m = (mask64(lo, hi, ',') | mask64(lo, hi, '\n')) & ~in;
            qmask[b] = q;
            qbefore[b] = quotes;
            quotes += __builtin_popcountll(q);
            u32 count = __builtin_popcountll(m), *out = at + seps;
            for (int k = 0; k < 8; ++k) out[k] = 64 * b + __builtin_ctzll(m), m = _blsr_u64(m);
            for (int k = 8; m; ++k) out[k] = 64 * b + __builtin_ctzll(m), m = _blsr_u64(m);
            seps += count;
        }
        // The final record may end at end of file without a terminator.
        if (p + 64 * blocks >= file_end) at[seps++] = file_end - p;
        auto quotes_before = [&](u32 x) { return qbefore[x / 64] + __builtin_popcountll(_bzhi_u64(qmask[x / 64], x % 64)); };
        const char* chunk = p;
        for (u32 i = 0; i + 5 <= seps && p < end; i += 5) {
            if (u8(*p) == 0xEF) p += 3;  // a byte order mark before the id
            const char *f1 = chunk + at[i], *f2 = chunk + at[i + 1], *f3 = chunk + at[i + 2];
            const char *f4 = chunk + at[i + 3], *eol = chunk + at[i + 4];
            u32 q1 = quotes_before(at[i]), q2 = quotes_before(at[i + 1]), q3 = quotes_before(at[i + 2]);
            u32 qname = q2 - q1, qnote = q3 - q2;
            u32 name_field = f2 - f1 - 1, note_field = f3 - f2 - 1;
            u32 quoted = qname != 0;
            const char* name_at = f1 + 1 + quoted;
            u32 name_raw = name_field - 2 * quoted;
            u32 name_length = name_field - qname / 2 - quoted;
            u32 note_length = note_field - qnote / 2 - (qnote != 0);
            const char* amount = f4 + 1;
            const char* amount_end = eol - (eol[-1] == '\r');
            bool negative = *amount == '-';
            Numbers num = numbers(p, f1, f3 + 1, f4, amount + negative, amount_end);
            u64 id = num.id, region = num.region;
            i64 cents = negative ? -i64(num.cents) : i64(num.cents);

            // Region ids are below 10^7 (fit u32); one past the table means none.
            u32 r = region < regions ? u32(region) : u32(regions);
            _mm_prefetch(reinterpret_cast<const char*>(country_of + r), _MM_HINT_T0);
            advance();
            u64 key = NameTable::key(name_at, name_raw);
            pending[n++ % LAG] = {key, r, u32(id), name_length, note_length, cents};
            names.prefetch(key);
            p = eol + 1;
        }
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
    // Small stacks: an 8 MB one could be backed by a whole 2 MB page once
    // touched, with transparent huge pages always on.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 << 10);
    pthread_setattr_default_np(&attr);
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
    NamesSeen seen;
    using CountryTotals = std::array<Totals, 256>;
    std::span<CountryTotals> totals;  // set up by each thread
    char* block = nullptr;  // the bitsets, then the totals
    size_t block_bytes = 0;
    auto number_countries = [&] {
        for (u32 k = 0; k < present.size(); ++k)
            if (present[k]) codes.push_back(k), index_of[k] = codes.size();
        if (codes.size() > 255) die("more than 255 countries");
        const size_t rows = (codes.size() + 1) * NamesSeen::WORDS;
        // On 2 MB pages: for 240 countries the bitsets and totals fill two
        // of them almost exactly (4,048 of 4,096 KB). A record's mark lands
        // at random in 3.9 MB, and on 4 KB pages nearly every one missed the
        // first-level TLB.
        block = zeroed<char>(block_bytes = rows * sizeof(u64) + nthreads * sizeof(CountryTotals), true);
        seen.bits = reinterpret_cast<std::atomic<u64>*>(block);
        totals = {reinterpret_cast<CountryTotals*>(block + rows * sizeof(u64)), size_t(nthreads)};
    };

    // records.csv: skip the BOM and header, then split after CRLFs into eight
    // pieces per thread, claimed in turn, so a thread whose pieces are slow
    // (more first sightings of names) is evened out by the others.
    size_t start = size ? static_cast<const char*>(memchr(records, '\n', size)) - records + 1 : 0;
    const int pieces = 8 * nthreads;
    std::vector<size_t> cut = split(records, start, size, pieces, "\r\n");
    NameTable names;

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
                populate_share(block, block_bytes, k, nthreads, HUGE_PAGE);
                for (const char* p = regions + rcut[k]; p < regions + rcut[k + 1];) {
                    auto [id, c] = region_line(p);
                    country_of[id] = index_of[c];
                }
            });
            new (&totals[t]) CountryTotals();
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
    if (!f) die("/out/aggregates.txt");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
