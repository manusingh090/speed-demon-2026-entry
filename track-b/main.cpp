// Speed Demon 2026, Track B (Index).
//
// The queries are readable up front, so we read them first and index only the
// terms they mention. One parallel pass over the memory-mapped corpus builds
// doc-level posting lists for those terms (sorted, deduplicated). BOOL queries
// are then evaluated left to right on sorted arrays, galloping when one side is
// much smaller. PHRASE queries intersect their terms' postings, rarest first,
// and re-tokenize only the surviving candidates to check adjacency.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using u32 = uint32_t;
using u64 = uint64_t;

constexpr int MAX_TERM = 32;  // query terms are 1 to 32 bytes
constexpr u32 NONE = 0xFFFFFFFFu;

// lower[c] is the lowercased byte for [A-Za-z0-9] and 0 for every separator.
struct Lower {
    uint8_t t[256] = {};
    Lower() {
        for (int c = '0'; c <= '9'; ++c) t[c] = c;
        for (int c = 'a'; c <= 'z'; ++c) t[c] = c;
        for (int c = 'A'; c <= 'Z'; ++c) t[c] = c + 32;
    }
};
const Lower LOWER;

struct File {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

File map_file(const std::string& path) {
    File f;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); _exit(1); }
    struct stat st;
    fstat(fd, &st);
    f.size = st.st_size;
    if (f.size) {
        void* p = mmap(nullptr, f.size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { perror("mmap"); _exit(1); }
        f.data = static_cast<const uint8_t*>(p);
    }
    close(fd);
    return f;
}

// A term key: up to 32 lowercase bytes, zero padded, so equal keys compare as
// four 64-bit words (no term contains a zero byte, so the padding encodes length).
struct Key {
    u64 w[4];
};

inline u64 hash_key(const Key& k) {
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u64 x : k.w) h = (h ^ x) * 0xBF58476D1CE4E5B9ull, h ^= h >> 31;
    return h;
}

inline bool same(const Key& a, const Key& b) {
    return ((a.w[0] ^ b.w[0]) | (a.w[1] ^ b.w[1]) | (a.w[2] ^ b.w[2]) | (a.w[3] ^ b.w[3])) == 0;
}

// Open-addressing table from term key to a dense term id. Each slot holds the
// top 32 hash bits and id + 1, so most misses never touch the key array.
class TermTable {
public:
    TermTable() { rehash(1 << 16); }
    u32 size() const { return keys_.size(); }

    u32 insert(const Key& k) {
        // Keep the load at or under 1/4 so probe chains stay short.
        if ((keys_.size() + 1) * 4 > slots_.size()) rehash(slots_.size() * 2);
        u64 h = hash_key(k);
        for (size_t i = h & mask_;; i = (i + 1) & mask_) {
            u64 s = slots_[i];
            if (s == 0) {
                keys_.push_back(k);
                slots_[i] = (h & 0xFFFFFFFF00000000ull) | keys_.size();
                return keys_.size() - 1;
            }
            if ((s >> 32) == (h >> 32) && same(keys_[(u32)s - 1], k)) return (u32)s - 1;
        }
    }

    u32 find(const Key& k) const {
        u64 h = hash_key(k);
        for (size_t i = h & mask_;; i = (i + 1) & mask_) {
            u64 s = slots_[i];
            if (s == 0) return NONE;
            if ((s >> 32) == (h >> 32) && same(keys_[(u32)s - 1], k)) return (u32)s - 1;
        }
    }

private:
    void rehash(size_t cap) {
        slots_.assign(cap, 0);
        mask_ = cap - 1;
        for (u32 id = 0; id < keys_.size(); ++id) {
            u64 h = hash_key(keys_[id]);
            size_t i = h & mask_;
            while (slots_[i]) i = (i + 1) & mask_;
            slots_[i] = (h & 0xFFFFFFFF00000000ull) | (id + 1);
        }
    }

    std::vector<u64> slots_;
    std::vector<Key> keys_;
    size_t mask_ = 0;
};

// Calls emit(term_id) for every token of text[b, e) that is a known term
// (NONE for tokens that are not). Tokens longer than 32 bytes cannot be terms.
template <class F>
inline void for_each_token(const uint8_t* q, const uint8_t* e, const TermTable& terms, F&& emit) {
    while (q < e) {
        uint8_t c = LOWER.t[*q];
        if (!c) { ++q; continue; }
        Key k{{0, 0, 0, 0}};
        auto* b = reinterpret_cast<uint8_t*>(k.w);
        int len = 0;
        do {
            if (len < MAX_TERM) b[len] = c;
            ++len;
            ++q;
        } while (q < e && (c = LOWER.t[*q]));
        emit(len <= MAX_TERM ? terms.find(k) : NONE);
    }
}

struct Query {
    bool phrase;
    std::vector<u32> terms;  // term ids
    std::vector<char> ops;   // BOOL only: 'A', 'O' or 'N' between terms
};

Key make_key(const uint8_t* p, size_t len) {
    Key k{{0, 0, 0, 0}};
    memcpy(k.w, p, std::min<size_t>(len, MAX_TERM));
    return k;
}

std::vector<Query> parse_queries(const File& f, TermTable& terms) {
    std::vector<Query> qs;
    const uint8_t* p = f.data;
    const uint8_t* end = f.data + f.size;
    while (p < end) {
        const uint8_t* nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
        if (!nl) nl = end;
        Query q;
        q.phrase = p[0] == 'P';
        const uint8_t* s = p + (q.phrase ? 8 : 5);  // past `PHRASE "` or `BOOL `
        const uint8_t* e = q.phrase ? nl - 1 : nl;  // drop the closing quote
        for (int word = 0; s < e; ++word) {
            const uint8_t* sp = static_cast<const uint8_t*>(memchr(s, ' ', e - s));
            if (!sp) sp = e;
            if (!q.phrase && word % 2 == 1) q.ops.push_back(static_cast<char>(*s));
            else q.terms.push_back(terms.insert(make_key(s, sp - s)));
            s = sp + 1;
        }
        qs.push_back(std::move(q));
        p = nl + 1;
    }
    return qs;
}

// Doc-level postings for every query term, in CSR form.
struct Index {
    u32 docs = 0;
    std::vector<u32> start;   // per term, offset into post; start[n] is the total
    std::vector<u32> post;    // doc ids, ascending within each term
    std::vector<u32> text;    // per doc: offset of its text, then of its end (2 per doc)
};

u32 parse_id(const uint8_t* p) {
    u32 v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

Index build_index(const File& f, const TermTable& terms, int nthreads) {
    Index ix;
    const u32 nterms = terms.size();
    const uint8_t* base = f.data;
    const size_t size = f.size;
    if (size) {
        // Ids are dense, so the last line's id gives the document count.
        size_t last = size - 1;
        while (last > 0 && base[last - 1] != '\n') --last;
        ix.docs = parse_id(base + last) + 1;
    }
    ix.text.resize(2 * size_t(ix.docs));

    // Each thread takes a byte range snapped to line starts. It records, in doc
    // order, the distinct term ids of each document followed by a NONE marker.
    std::vector<size_t> cut(nthreads + 1);
    for (int t = 0; t <= nthreads; ++t) {
        size_t c = size * t / nthreads;
        while (c > 0 && c < size && base[c - 1] != '\n') ++c;
        cut[t] = c;
    }
    std::vector<std::vector<u32>> seen(nthreads);
    std::vector<std::vector<u32>> count(nthreads);
    std::vector<u32> first_doc(nthreads);
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            auto& out = seen[t];
            auto& cnt = count[t];
            cnt.assign(nterms, 0);
            std::vector<u32> last_doc(nterms, NONE);
            const uint8_t* p = base + cut[t];
            const uint8_t* end = base + cut[t + 1];
            u32 doc = p < end ? parse_id(p) : 0;
            first_doc[t] = doc;
            out.reserve((end - p) / 12);
            for (; p < end; ++doc) {
                const uint8_t* nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
                const uint8_t* tx = static_cast<const uint8_t*>(memchr(p, '\t', nl - p)) + 1;
                ix.text[2 * size_t(doc)] = tx - base;
                ix.text[2 * size_t(doc) + 1] = nl - base;
                for_each_token(tx, nl, terms, [&](u32 id) {
                    if (id != NONE && last_doc[id] != doc) {
                        last_doc[id] = doc;
                        out.push_back(id);
                        ++cnt[id];
                    }
                });
                out.push_back(NONE);
                p = nl + 1;
            }
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();

    // Turn the per-thread counts into write cursors. Threads cover ascending doc
    // ranges, so laying them out in thread order keeps every list sorted.
    ix.start.resize(nterms + 1);
    u32 total = 0;
    for (u32 id = 0; id < nterms; ++id) {
        ix.start[id] = total;
        for (int t = 0; t < nthreads; ++t) {
            u32 c = count[t][id];
            count[t][id] = total;
            total += c;
        }
    }
    ix.start[nterms] = total;
    ix.post.resize(total);
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            auto& cur = count[t];
            u32 doc = first_doc[t];
            for (u32 id : seen[t]) {
                if (id == NONE) ++doc;
                else ix.post[cur[id]++] = doc;
            }
            std::vector<u32>().swap(seen[t]);
        });
    }
    for (auto& th : pool) th.join();
    return ix;
}

struct Span {
    const u32* p;
    size_t n;
};

// First index i >= lo with a[i] >= x, searching outward from lo.
inline size_t gallop(const u32* a, size_t lo, size_t n, u32 x) {
    size_t step = 1, hi = lo;
    while (hi < n && a[hi] < x) lo = hi + 1, hi += step, step <<= 1;
    return std::lower_bound(a + lo, a + std::min(hi, n), x) - a;
}

// Set operations on sorted arrays. Merging is linear; when one side is far
// smaller we gallop through the larger one instead.
void op_and(const std::vector<u32>& a, Span b, std::vector<u32>& out) {
    out.clear();
    if (a.size() * 16 < b.n) {
        size_t j = 0;
        for (u32 x : a) {
            j = gallop(b.p, j, b.n, x);
            if (j == b.n) break;
            if (b.p[j] == x) out.push_back(x);
        }
    } else if (b.n * 16 < a.size()) {
        size_t i = 0;
        for (size_t k = 0; k < b.n; ++k) {
            i = gallop(a.data(), i, a.size(), b.p[k]);
            if (i == a.size()) break;
            if (a[i] == b.p[k]) out.push_back(b.p[k]);
        }
    } else {
        std::set_intersection(a.begin(), a.end(), b.p, b.p + b.n, std::back_inserter(out));
    }
}

void op_not(const std::vector<u32>& a, Span b, std::vector<u32>& out) {
    out.clear();
    if (a.size() * 16 < b.n) {
        size_t j = 0;
        for (u32 x : a) {
            j = gallop(b.p, j, b.n, x);
            if (j == b.n || b.p[j] != x) out.push_back(x);
        }
    } else {
        std::set_difference(a.begin(), a.end(), b.p, b.p + b.n, std::back_inserter(out));
    }
}

void op_or(const std::vector<u32>& a, Span b, std::vector<u32>& out) {
    out.clear();
    std::set_union(a.begin(), a.end(), b.p, b.p + b.n, std::back_inserter(out));
}

class Solver {
public:
    Solver(const Index& ix, const TermTable& terms, const File& docs)
        : ix_(ix), terms_(terms), docs_(docs) {}

    // Writes one result line (with its LF) to out, returns its length.
    int answer(const Query& q, char* out) {
        if (q.phrase) phrase(q);
        else boolean(q);
        return format(out);
    }

private:
    Span postings(u32 id) const {
        return {ix_.post.data() + ix_.start[id], size_t(ix_.start[id + 1] - ix_.start[id])};
    }

    void boolean(const Query& q) {
        Span s = postings(q.terms[0]);
        cur_.assign(s.p, s.p + s.n);
        for (size_t i = 0; i < q.ops.size(); ++i) {
            Span b = postings(q.terms[i + 1]);
            switch (q.ops[i]) {
                case 'A': op_and(cur_, b, tmp_); break;
                case 'O': op_or(cur_, b, tmp_); break;
                default: op_not(cur_, b, tmp_); break;
            }
            cur_.swap(tmp_);
        }
    }

    void phrase(const Query& q) {
        // Candidates hold every phrase term; intersect rarest first.
        order_.assign(q.terms.begin(), q.terms.end());
        std::sort(order_.begin(), order_.end());
        order_.erase(std::unique(order_.begin(), order_.end()), order_.end());
        std::sort(order_.begin(), order_.end(),
                  [&](u32 a, u32 b) { return postings(a).n < postings(b).n; });
        Span s = postings(order_[0]);
        cur_.assign(s.p, s.p + s.n);
        for (size_t i = 1; i < order_.size() && !cur_.empty(); ++i) {
            op_and(cur_, postings(order_[i]), tmp_);
            cur_.swap(tmp_);
        }
        // Keep the candidates whose token sequence contains the phrase.
        const size_t k = q.terms.size();
        size_t kept = 0;
        for (u32 doc : cur_) {
            toks_.clear();
            const uint8_t* b = docs_.data + ix_.text[2 * size_t(doc)];
            const uint8_t* e = docs_.data + ix_.text[2 * size_t(doc) + 1];
            bool hit = false;
            for_each_token(b, e, terms_, [&](u32 id) {
                toks_.push_back(id);
                if (!hit && toks_.size() >= k &&
                    std::equal(q.terms.begin(), q.terms.end(), toks_.end() - k))
                    hit = true;
            });
            if (hit) cur_[kept++] = doc;
        }
        cur_.resize(kept);
    }

    int format(char* out) const {
        u64 sum = 0;
        for (u32 x : cur_) sum += x;
        char* p = out;
        p = put(p, cur_.size());
        *p++ = ' ';
        p = put(p, sum);
        for (size_t i = 0; i < cur_.size() && i < 10; ++i) {
            *p++ = ' ';
            p = put(p, cur_[i]);
        }
        *p++ = '\n';
        return p - out;
    }

    static char* put(char* p, u64 v) {
        char tmp[20];
        int n = 0;
        do tmp[n++] = '0' + v % 10, v /= 10; while (v);
        while (n) *p++ = tmp[--n];
        return p;
    }

    const Index& ix_;
    const TermTable& terms_;
    const File& docs_;
    std::vector<u32> cur_, tmp_, order_, toks_;
};

}  // namespace

int main() {
    const int nthreads = std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
    File qf = map_file("/data/queries.txt");
    File df = map_file("/data/docs.txt");

    TermTable terms;
    std::vector<Query> queries = parse_queries(qf, terms);
    Index ix = build_index(df, terms, nthreads);

    // Answer queries in parallel; each result goes to its own fixed-size slot
    // (the longest line is 6 + 12 + 10 * 7 digits and spaces, well under 128).
    constexpr int SLOT = 128;
    const size_t nq = queries.size();
    std::vector<char> slots(nq * SLOT);
    std::vector<uint8_t> lens(nq);
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&] {
            Solver solver(ix, terms, df);
            for (size_t i; (i = next.fetch_add(16)) < nq;)
                for (size_t j = i; j < std::min(nq, i + 16); ++j)
                    lens[j] = solver.answer(queries[j], &slots[j * SLOT]);
        });
    }
    for (auto& th : pool) th.join();

    std::string out;
    out.reserve(nq * 32);
    for (size_t i = 0; i < nq; ++i) out.append(&slots[i * SLOT], lens[i]);
    FILE* f = fopen("/out/results.txt", "wb");
    if (!f) { perror("/out/results.txt"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
