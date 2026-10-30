// Speed Demon 2026, Track A (Ingest).
//
// The log is memory-mapped and split into line-aligned ranges, one per thread.
// Each thread parses its range with fixed byte offsets (the timestamp is always
// 27 bytes and the 72-hour span is fixed, so the hour index comes straight from
// the day and hour digits), interns endpoints in a small private hash table and
// files (latency, endpoint, 5xx flag) into a block list for that hour. The
// private endpoint tables are merged and sorted once, so an endpoint's global id
// is its rank in byte order. Then every hour is finished on its own: a counting
// sort by endpoint rank lays its ~43k latencies out in L2-sized scratch, three
// nth_element calls per group give the percentiles, and the lines are formatted
// in output order. A writer thread writes each hour as soon as it is done.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr int HOURS = 72;          // 2026-10-30T18:00Z to 2026-11-02T17:59:59Z
constexpr u16 ERR_BIT = 1u << 15;  // 5xx flag, above the local endpoint id

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

// The first byte c at or after s (one is known to follow).
const char* find(const char* s, char c) {
    for (;; s += 32)
        if (u32 m = match32(s, c)) return s + __builtin_ctz(m);
}

// The value of the len (1 to 10) decimal digits that end just before end,
// without a loop: the 16 bytes before end, minus '0', with the bytes ahead of
// the number zeroed, are folded 2, 4 and 8 digits at a time.
u32 parse_digits(const char* end, u32 len) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 16));
    v = _mm_subs_epu8(v, _mm_set1_epi8('0'));
    __m128i lane = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
    v = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));  // byte pairs (10, 1)
    v = _mm_madd_epi16(v, _mm_set1_epi32(0x00010064));  // word pairs (100, 1)
    v = _mm_packus_epi32(v, v);
    v = _mm_madd_epi16(v, _mm_set1_epi32(0x00012710));  // word pairs (10000, 1)
    u64 high = u32(_mm_cvtsi128_si32(v)), low = u32(_mm_extract_epi32(v, 1));
    return u32(high * 100000000 + low);
}

// Endpoint interning: open addressing on a CRC of the endpoint's first and last
// eight bytes (masked to the endpoint when it is shorter; the line around it
// keeps both loads in bounds). Ids follow first sight; names point into the log.
class EndpointTable {
public:
    static constexpr u32 SLOTS = 1 << 13, MAX = SLOTS / 2;  // a seed has 1,842
    std::vector<std::string_view> names;

    u32 intern(const char* s, u32 len) {
        u64 a = load8(s), b = len >= 8 ? load8(s + len - 8) : 0;
        if (len < 8) a &= (u64(1) << 8 * len) - 1;
        for (u32 i = _mm_crc32_u64(_mm_crc32_u64(len, a), b);; ++i) {
            u32& slot = slots_[i % SLOTS];
            if (slot == 0) {
                if (names.size() == MAX) { fputs("too many endpoints\n", stderr); _exit(1); }
                names.emplace_back(s, len);
                return (slot = names.size()) - 1;
            }
            std::string_view n = names[slot - 1];
            if (n.size() == len && memcmp(n.data(), s, len) == 0) return slot - 1;
        }
    }

private:
    std::vector<u32> slots_ = std::vector<u32>(SLOTS);  // id + 1, 0 when empty
};

// A chunk of one hour's requests from one thread: latencies and keys (local
// endpoint id plus the 5xx flag) in parallel arrays.
struct Block {
    static constexpr u32 CAP = 2048;
    Block* next;
    u32 n = 0;
    u32 lat[CAP];
    u16 key[CAP];
};

struct Worker {
    EndpointTable endpoints;
    Block* head[HOURS] = {};

    void parse(const char* p, const char* end) {
        while (p < end) {
            // YYYY-MM-DDTHH: month digit at 6, day at 8-9, hour at 11-12. The
            // day index counts Oct 30, Oct 31, Nov 1, Nov 2.
            int day = (p[8] - '0') * 10 + p[9] - '0';
            int hour = (p[11] - '0') * 10 + p[12] - '0';
            int h = (p[6] == '0' ? day - 30 : day + 1) * 24 + hour - 18;

            // One scan of the 64 bytes after the timestamp finds the end of the
            // method and, for all but the longest lines, the end of the endpoint
            // and of the line. The first space always lies inside the window.
            const char* m = p + 28;
            u64 sp = match32(m, ' ') | u64(match32(m + 32, ' ')) << 32;
            u64 nl = match32(m, '\n') | u64(match32(m + 32, '\n')) << 32;
            const char* ep = m + __builtin_ctzll(sp) + 1;
            const char* ee = nl ? m + __builtin_ctzll(sp & (sp - 1)) : find(ep, ' ');
            const char* eol = nl ? m + __builtin_ctzll(nl) : find(ee + 5, '\n');
            u16 err = ee[1] == '5' ? ERR_BIT : 0;

            Block*& b = head[h];
            if (!b || b->n == Block::CAP) {
                Block* fresh = new Block;
                fresh->next = b;
                b = fresh;
            }
            b->lat[b->n] = parse_digits(eol, eol - ee - 5);
            b->key[b->n++] = endpoints.intern(ep, ee - ep) | err;
            p = eol + 1;
        }
    }
};

// "YYYY-MM-DDTHH:00:00Z " for hour index h, plus a terminating zero.
void hour_label(int h, char* o) {
    int day = (h + 18) / 24;  // Oct 30, Oct 31, Nov 1, Nov 2
    sprintf(o, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
            day < 2 ? 30 + day : day - 1, (h + 18) % 24);
}

template <class F>
void run_threads(int n, F&& body) {
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) pool.emplace_back(body, t);
    for (auto& th : pool) th.join();
}

int main() {
    const int nthreads = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
    int fd = open("/data/requests.log", O_RDONLY);
    FILE* out = fopen("/out/summary.txt", "wb");
    if (fd < 0 || !out) { perror("open"); return 1; }

    // Map the log over a spare zero page, so the 64-byte scans stay in bounds.
    size_t size = lseek(fd, 0, SEEK_END);
    auto* data = static_cast<char*>(
        mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (size && mmap(data, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // Line-aligned ranges, one per thread.
    std::vector<size_t> cut(nthreads + 1, size);
    cut[0] = 0;
    for (int t = 1; t < nthreads; ++t) {
        cut[t] = std::max(cut[t - 1], size * t / nthreads);
        while (cut[t] < size && data[cut[t] - 1] != '\n') ++cut[t];
    }
    std::vector<Worker> workers(nthreads);
    run_threads(nthreads, [&](int t) { workers[t].parse(data + cut[t], data + cut[t + 1]); });

    // Merge the local tables and sort the names: global ids become ranks in
    // unsigned byte order (a proper prefix first), which is output order.
    EndpointTable global;
    std::vector<std::vector<u32>> rank(nthreads);
    for (int t = 0; t < nthreads; ++t)
        for (std::string_view n : workers[t].endpoints.names)
            rank[t].push_back(global.intern(n.data(), n.size()));
    std::vector<std::string_view> names = global.names;
    std::sort(names.begin(), names.end());
    std::vector<u32> rank_of(names.size());
    for (u32 i = 0; i < names.size(); ++i)
        rank_of[global.intern(names[i].data(), names[i].size())] = i;
    for (auto& r : rank)
        for (u32& id : r) id = rank_of[id];

    // Hours are claimed in order by the pool; a writer thread writes them in
    // order as each one finishes.
    std::vector<std::string> text(HOURS);
    std::vector<std::atomic<bool>> done(HOURS);
    std::thread writer([&] {
        for (int h = 0; h < HOURS; ++h) {
            done[h].wait(false);
            fwrite(text[h].data(), 1, text[h].size(), out);
            std::string().swap(text[h]);
        }
    });
    const u32 ne = names.size();
    std::atomic<int> next{0};
    run_threads(nthreads, [&](int) {
        std::vector<u32> count, errors, cursor, lat;
        for (int h; (h = next++) < HOURS;) {
            // Counting sort of the hour's latencies by endpoint rank.
            count.assign(ne + 1, 0);
            errors.assign(ne, 0);
            for (int t = 0; t < nthreads; ++t)
                for (Block* b = workers[t].head[h]; b; b = b->next)
                    for (u32 i = 0; i < b->n; ++i) {
                        u32 e = rank[t][b->key[i] & ~ERR_BIT];
                        ++count[e + 1];
                        errors[e] += b->key[i] >> 15;
                    }
            for (u32 e = 0; e < ne; ++e) count[e + 1] += count[e];
            cursor.assign(count.begin(), count.end() - 1);
            lat.resize(count[ne]);
            for (int t = 0; t < nthreads; ++t)
                for (Block* b = workers[t].head[h]; b; delete std::exchange(b, b->next))
                    for (u32 i = 0; i < b->n; ++i)
                        lat[cursor[rank[t][b->key[i] & ~ERR_BIT]]++] = b->lat[i];

            char label[22], line[256];
            hour_label(h, label);
            for (u32 e = 0; e < ne; ++e) {
                u32* a = lat.data() + count[e];
                u32 n = count[e + 1] - count[e];
                if (n == 0) continue;
                // Nearest rank p is index ceil(p * n / 100) - 1. A selection may
                // reorder its whole range, so read each value before narrowing
                // to the tail for the next one.
                u32 v[6] = {n, 0, 0, 0, errors[e], u32(u64(errors[e]) * 1000000 / n)};
                u32 lo = 0, k = 1;
                for (u32 p : {50u, 95u, 99u}) {
                    u32 r = (p * n + 99) / 100 - 1;
                    std::nth_element(a + lo, a + r, a + n);
                    v[k++] = a[lo = r];
                }
                char* o = std::copy(names[e].begin(), names[e].end(), std::copy_n(label, 21, line));
                for (u32 x : v) {
                    *o++ = ' ';
                    o = std::to_chars(o, o + 10, x).ptr;
                }
                *o++ = '\n';
                text[h].append(line, o);
            }
            done[h] = true;
            done[h].notify_one();
        }
    });
    writer.join();
    fclose(out);
    return 0;
}
