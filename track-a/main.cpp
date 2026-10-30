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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr int HOURS = 72;               // 2026-10-30T18:00Z to 2026-11-02T17:59:59Z
constexpr u16 ERR_BIT = 1u << 15;       // 5xx flag, above a 15-bit local endpoint id
constexpr u32 MAX_LOCAL = ERR_BIT - 1;  // a seed has 1,842 endpoints, far below this

struct File {
    const char* data = nullptr;
    size_t size = 0;
};

// Map the file with at least one readable page after it (an anonymous zero
// page underneath the file mapping), so 32-byte loads near the end are safe.
File map_file(const char* path) {
    File f;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); _exit(1); }
    struct stat st;
    fstat(fd, &st);
    f.size = st.st_size;
    if (f.size) {
        void* base = mmap(nullptr, f.size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        void* p = mmap(base, f.size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
        if (base == MAP_FAILED || p == MAP_FAILED) { perror("mmap"); _exit(1); }
        f.data = static_cast<const char*>(p);
    }
    close(fd);
    return f;
}

inline u64 load8(const char* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

// Hash of an endpoint from its first and last eight bytes and its length. Every
// endpoint is followed by at least " 100 0\n" and preceded by the timestamp, so
// both loads stay inside the line; for short endpoints the first load is masked
// to the endpoint's own bytes and stands in for the second.
inline u64 hash_endpoint(const char* s, u32 len) {
    u64 a = load8(s), b;
    if (len >= 8) {
        b = load8(s + len - 8);
    } else {
        a &= (u64(1) << (8 * len)) - 1;
        b = a;
    }
    u64 h = (a * 0x9E3779B97F4A7C15ull) ^ (b + len) * 0xC2B2AE3D27D4EB4Full;
    return h ^ (h >> 29);
}

inline __m256i load32(const char* p) { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)); }

inline u32 match32(const char* p, char c) {
    return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(load32(p), _mm256_set1_epi8(c))));
}

// Byte equality of two len-byte strings, 32 bytes at a time. Both point into
// the mapped log, which has a padding page after it, so over-reading is safe.
inline bool same_bytes(const char* a, const char* b, u32 len) {
    for (u32 i = 0; i < len; i += 32) {
        u32 diff = ~u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(load32(a + i), load32(b + i))));
        if (len - i < 32) diff &= (1u << (len - i)) - 1;
        if (diff) return false;
    }
    return true;
}

// Endpoint interning: open addressing on the hash, ids in order of first sight.
// Endpoint bytes stay in the mapping; slots keep a pointer to them.
class EndpointTable {
public:
    EndpointTable() { slots_.assign(1 << 12, Slot{}); }

    u32 intern(const char* s, u32 len) {
        u64 h = hash_endpoint(s, len);
        size_t mask = slots_.size() - 1;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            Slot& sl = slots_[i];
            if (sl.id == EMPTY) {
                sl = {h, s, len, u32(names_.size())};
                names_.emplace_back(s, len);
                if (names_.size() * 2 > slots_.size()) grow();
                return u32(names_.size() - 1);
            }
            if (sl.hash == h && sl.len == len && same_bytes(sl.name, s, len)) return sl.id;
        }
    }

    const std::vector<std::string_view>& names() const { return names_; }

private:
    static constexpr u32 EMPTY = 0xFFFFFFFFu;
    struct Slot {
        u64 hash = 0;
        const char* name = nullptr;
        u32 len = 0;
        u32 id = EMPTY;
    };

    void grow() {
        std::vector<Slot> old(slots_.size() * 2);
        old.swap(slots_);
        size_t mask = slots_.size() - 1;
        for (const Slot& sl : old) {
            if (sl.id == EMPTY) continue;
            size_t i = sl.hash & mask;
            while (slots_[i].id != EMPTY) i = (i + 1) & mask;
            slots_[i] = sl;
        }
    }

    std::vector<Slot> slots_;
    std::vector<std::string_view> names_;
};

// A chunk of one hour's requests from one thread: latencies and keys (local
// endpoint id plus the 5xx flag) in parallel arrays.
struct Block {
    static constexpr u32 CAP = 2048;
    Block* next;
    u32 n;
    u32 lat[CAP];
    u16 key[CAP];
};

// The position of the first byte c at or after s (one is known to follow).
inline const char* find(const char* s, char c) {
    for (;; s += 32)
        if (u32 m = match32(s, c)) return s + __builtin_ctz(m);
}

// Bit masks of the spaces and newlines in the 64 bytes at s.
inline void scan64(const char* s, u64& spaces, u64& newlines) {
    spaces = match32(s, ' ') | u64(match32(s + 32, ' ')) << 32;
    newlines = match32(s, '\n') | u64(match32(s + 32, '\n')) << 32;
}

// The value of the len (1 to 10) decimal digits that end just before end,
// without a loop: the 16 bytes before end, minus '0', with the bytes ahead of
// the number zeroed, are folded 2, 4 and 8 digits at a time.
inline u32 parse_digits(const char* end, u32 len) {
    __m128i v = _mm_subs_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(end - 16)),
                              _mm_set1_epi8('0'));
    const __m128i lane = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
    v = _mm_maddubs_epi16(v, _mm_setr_epi8(10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1, 10, 1));
    v = _mm_madd_epi16(v, _mm_setr_epi16(100, 1, 100, 1, 100, 1, 100, 1));
    v = _mm_packus_epi32(v, v);
    v = _mm_madd_epi16(v, _mm_setr_epi16(10000, 1, 10000, 1, 10000, 1, 10000, 1));
    return u32(u64(u32(_mm_cvtsi128_si32(v))) * 100000000 + u32(_mm_extract_epi32(v, 1)));
}

struct Worker {
    EndpointTable endpoints;
    Block* head[HOURS] = {};

    void add(u32 hour, u32 lat, u16 key) {
        Block* b = head[hour];
        if (!b || b->n == Block::CAP) {
            Block* nb = static_cast<Block*>(malloc(sizeof(Block)));
            nb->next = b;
            nb->n = 0;
            head[hour] = b = nb;
        }
        b->lat[b->n] = lat;
        b->key[b->n++] = key;
    }

    void parse(const char* p, const char* end) {
        while (p < end) {
            // YYYY-MM-DDTHH: month digit at 6, day at 8-9, hour at 11-12.
            int day = (p[8] - '0') * 10 + (p[9] - '0');
            int hour = (p[11] - '0') * 10 + (p[12] - '0');
            int day_index = p[6] == '0' ? day - 30 : day + 1;  // Oct 30, 31, Nov 1, 2
            u32 hour_index = u32(day_index * 24 + hour - 18);

            // One scan of the 64 bytes after the timestamp finds the end of the
            // method and, for all but the longest lines, the end of the endpoint
            // and of the line. The first space always lies inside the window.
            const char* m = p + 28;
            u64 spaces, newlines;
            scan64(m, spaces, newlines);
            const char* ep = m + __builtin_ctzll(spaces) + 1;
            const char *sp, *nl;
            if (newlines) {
                sp = m + __builtin_ctzll(spaces & (spaces - 1));
                nl = m + __builtin_ctzll(newlines);
            } else {
                sp = find(ep, ' ');
                nl = find(sp + 5, '\n');
            }
            u32 id = endpoints.intern(ep, u32(sp - ep));
            if (id > MAX_LOCAL) { fputs("too many endpoints\n", stderr); _exit(1); }

            u16 err = sp[1] == '5' ? ERR_BIT : 0;
            u32 lat = parse_digits(nl, u32(nl - (sp + 5)));
            p = nl + 1;

            add(hour_index, lat, u16(id | err));
        }
    }
};

char* put_u32(char* o, u32 v) {
    char buf[10];
    int n = 0;
    do {
        buf[n++] = char('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) *o++ = buf[--n];
    return o;
}

void write_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { perror("write"); _exit(1); }
        p += w;
        n -= size_t(w);
    }
}

// "YYYY-MM-DDTHH:00:00Z " for an hour index.
void hour_label(int h, char* o) {
    static const char* const DAYS[] = {"2026-10-30", "2026-10-31", "2026-11-01", "2026-11-02"};
    int abs_hour = h + 18;
    memcpy(o, DAYS[abs_hour / 24], 10);
    o[10] = 'T';
    o[11] = char('0' + abs_hour % 24 / 10);
    o[12] = char('0' + abs_hour % 24 % 10);
    memcpy(o + 13, ":00:00Z ", 8);
}

// Everything needed to finish one hour, shared read-only by the hour threads.
struct Summary {
    std::vector<Worker>& workers;
    std::vector<std::vector<u32>>& rank;  // per thread: local endpoint id -> global rank
    std::vector<std::string_view>& names; // global rank -> endpoint bytes

    // Scratch per hour thread, reused across hours.
    struct Scratch {
        std::vector<u32> count, errors, lat;
    };

    void hour(int h, Scratch& s, std::string& text) const {
        const u32 ne = u32(names.size());
        s.count.assign(ne + 1, 0);
        s.errors.assign(ne, 0);
        for (size_t t = 0; t < workers.size(); ++t)
            for (const Block* b = workers[t].head[h]; b; b = b->next)
                for (u32 i = 0; i < b->n; ++i) {
                    u32 g = rank[t][b->key[i] & MAX_LOCAL];
                    ++s.count[g + 1];
                    s.errors[g] += b->key[i] >> 15;
                }
        for (u32 e = 0; e < ne; ++e) s.count[e + 1] += s.count[e];
        s.lat.resize(s.count[ne]);
        std::vector<u32> cursor(s.count.begin(), s.count.end() - 1);
        for (size_t t = 0; t < workers.size(); ++t)
            for (Block* b = workers[t].head[h]; b;) {
                for (u32 i = 0; i < b->n; ++i) s.lat[cursor[rank[t][b->key[i] & MAX_LOCAL]]++] = b->lat[i];
                Block* next = b->next;
                free(b);
                b = next;
            }

        char label[21];
        hour_label(h, label);
        for (u32 e = 0; e < ne; ++e) {
            u32* a = s.lat.data() + s.count[e];
            u32 n = s.count[e + 1] - s.count[e];
            if (n == 0) continue;
            // Each selection may reorder its own range, so read its value before
            // narrowing to the tail for the next one.
            u32 r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
            std::nth_element(a, a + r50, a + n);
            u32 p50 = a[r50];
            std::nth_element(a + r50, a + r95, a + n);
            u32 p95 = a[r95];
            std::nth_element(a + r95, a + r99, a + n);
            u32 p99 = a[r99];

            char buf[256];
            char* o = buf;
            memcpy(o, label, 21);
            o += 21;
            memcpy(o, names[e].data(), names[e].size());
            o += names[e].size();
            u32 errs = s.errors[e];
            for (u32 v : {n, p50, p95, p99, errs, u32(u64(errs) * 1000000 / n)}) {
                *o++ = ' ';
                o = put_u32(o, v);
            }
            *o++ = '\n';
            text.append(buf, o);
        }
    }
};

template <class F>
void run_threads(int n, F&& body) {
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) pool.emplace_back(body, t);
    for (auto& th : pool) th.join();
}

}  // namespace

int main() {
    const int nthreads = int(std::max(1u, std::min(12u, std::thread::hardware_concurrency())));
    File f = map_file("/data/requests.log");
    int out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror("/out/summary.txt"); return 1; }
    if (f.size == 0) return 0;

    // Line-aligned ranges, one per thread.
    std::vector<size_t> cut(nthreads + 1, f.size);
    cut[0] = 0;
    for (int t = 1; t < nthreads; ++t) {
        size_t c = std::max(cut[t - 1], f.size * t / nthreads);
        while (c < f.size && f.data[c - 1] != '\n') ++c;
        cut[t] = c;
    }
    std::vector<Worker> workers(nthreads);
    run_threads(nthreads, [&](int t) { workers[t].parse(f.data + cut[t], f.data + cut[t + 1]); });

    // Global endpoint ids are ranks in byte order (string_view compares as
    // unsigned bytes, a proper prefix first), so groups come out in output order.
    EndpointTable global;
    std::vector<std::vector<u32>> rank(nthreads);
    for (int t = 0; t < nthreads; ++t)
        for (std::string_view n : workers[t].endpoints.names())
            rank[t].push_back(global.intern(n.data(), u32(n.size())));
    std::vector<std::string_view> names = global.names();
    std::vector<u32> order(names.size()), rank_of(names.size());
    for (u32 i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return names[a] < names[b]; });
    for (u32 i = 0; i < order.size(); ++i) rank_of[order[i]] = i;
    for (auto& r : rank)
        for (u32& id : r) id = rank_of[id];
    std::sort(names.begin(), names.end());

    // Hours are claimed in order by the pool; a writer thread writes them in order
    // as each one finishes.
    Summary summary{workers, rank, names};
    std::vector<std::string> text(HOURS);
    std::vector<std::atomic<bool>> done(HOURS);
    std::atomic<int> next{0};
    std::thread writer([&] {
        for (int h = 0; h < HOURS; ++h) {
            done[h].wait(false);
            write_all(out, text[h].data(), text[h].size());
            std::string().swap(text[h]);
        }
    });
    run_threads(nthreads, [&](int) {
        Summary::Scratch scratch;
        for (int h; (h = next.fetch_add(1)) < HOURS;) {
            summary.hour(h, scratch, text[h]);
            done[h].store(true);
            done[h].notify_one();
        }
    });
    writer.join();
    close(out);
    return 0;
}
