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
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <span>
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

// Maps the file over a spare zero page, so 64-byte loads near its end are safe.
File map_file(const std::string& path) {
    File f;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { perror(path.c_str()); _exit(1); }
    struct stat st;
    fstat(fd, &st);
    f.size = st.st_size;
    void* p = mmap(nullptr, f.size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (f.size && mmap(p, f.size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("mmap");
        _exit(1);
    }
    f.data = static_cast<const uint8_t*>(p);
    close(fd);
    return f;
}

// A term key: up to 32 lowercase bytes, zero padded, so equal keys compare as
// four 64-bit words (no term contains a zero byte, so the padding encodes length).
struct alignas(32) Key {
    u64 w[4];
};

// Two independent multiplies; almost every token fits in the first two words.
inline u64 hash_key(const Key& k) {
    u64 h = (k.w[0] + k.w[2] * 0x94D049BB133111EBull) * 0x9E3779B97F4A7C15ull ^
            (k.w[1] + k.w[3] * 0xD6E8FEB86659FD93ull) * 0xBF58476D1CE4E5B9ull;
    return h ^ h >> 29;
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
    const uint8_t* bytes(u32 id) const { return reinterpret_cast<const uint8_t*>(keys_[id].w); }
    size_t length(u32 id) const { return lens_[id]; }

    u32 insert(const Key& k) {
        // Keep the load at or under 1/2 so probe chains stay short.
        if ((keys_.size() + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        u64 h = hash_key(k);
        for (size_t i = h & mask_;; i = (i + 1) & mask_) {
            u64 s = slots_[i];
            if (s == 0) {
                keys_.push_back(k);
                lens_.push_back(strnlen(reinterpret_cast<const char*>(k.w), MAX_TERM));
                slots_[i] = (h & 0xFFFFFFFF00000000ull) | keys_.size();
                return keys_.size() - 1;
            }
            if ((s >> 32) == (h >> 32) && same(keys_[(u32)s - 1], k)) return (u32)s - 1;
        }
    }

    // Old id i becomes new_id[i]; keys move with their ids.
    void renumber(const std::vector<u32>& new_id) {
        std::vector<Key> keys(keys_.size());
        std::vector<uint8_t> lens(lens_.size());
        for (u32 i = 0; i < keys_.size(); ++i) keys[new_id[i]] = keys_[i], lens[new_id[i]] = lens_[i];
        keys_.swap(keys);
        lens_.swap(lens);
        rehash(slots_.size());
    }

    void prefetch(u64 h) const { _mm_prefetch(reinterpret_cast<const char*>(&slots_[h & mask_]), _MM_HINT_T0); }

    // h is hash_key(k), computed (and prefetched) earlier by the caller.
    u32 find(const Key& k, u64 h) const {
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
    std::vector<uint8_t> lens_;
    size_t mask_ = 0;
};

// Bit i is set when byte i of the 64 at p is a token byte, [A-Za-z0-9]: after
// OR-ing 0x20 a letter lies in 'a'..'z', and a digit lies in '0'..'9' as is.
inline u64 token_bytes(const uint8_t* p) {
    auto half = [](const uint8_t* q) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q));
        __m256i letter = _mm256_sub_epi8(_mm256_or_si256(v, _mm256_set1_epi8(0x20)), _mm256_set1_epi8('a'));
        __m256i digit = _mm256_sub_epi8(v, _mm256_set1_epi8('0'));
        __m256i zero = _mm256_setzero_si256();
        __m256i in = _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(letter, _mm256_set1_epi8(25)), zero),
                                     _mm256_cmpeq_epi8(_mm256_subs_epu8(digit, _mm256_set1_epi8(9)), zero));
        return u32(_mm256_movemask_epi8(in));
    };
    return half(p) | u64(half(p + 32)) << 32;
}

// Calls emit(term_id) for every token of text [q, e) that is a known term
// (NONE for tokens that are not; tokens longer than 32 bytes are skipped).
// Token bounds come from one class mask per 64-byte window (a token that runs
// into the window's end starts the next window). The key is read from the
// text 8 bytes at a time: token bytes are all [A-Za-z0-9], so OR-ing 0x20
// lowercases letters and leaves digits alone. Each key's table slot is
// prefetched and the key looked up LAG tokens later, so the misses overlap.
template <class F>
inline void for_each_token(const uint8_t* q, const uint8_t* e, const TermTable& terms, F&& emit) {
    constexpr size_t LAG = 8;
    Key keys[LAG];
    u64 hashes[LAG];
    size_t n = 0;
    u64 m = 0;  // token bytes of the window at q not yet consumed
    for (bool fresh = true; q < e;) {
        if (fresh) {
            m = token_bytes(q);
            if (e - q < 64) m &= (1ull << (e - q)) - 1;
            fresh = false;
        }
        if (m == 0) { q += 64; fresh = true; continue; }
        const uint8_t* s = q + __builtin_ctzll(m);
        const u64 after = ~m & (~0ull << __builtin_ctzll(m));  // bytes past the token start that end it
        if (after == 0 && s != q) { q = s; fresh = true; continue; }  // re-window at the token
        const uint8_t* t = after ? q + __builtin_ctzll(after) : s + 64;
        while (t < e && LOWER.t[*t]) ++t;  // only a token of 64 bytes or more
        m &= after ? ~0ull << __builtin_ctzll(after) : 0;
        if (t > q + 64) { q = t; fresh = true; }
        const size_t len = t - s;
        if (len > MAX_TERM) continue;
        Key k;
        for (size_t i = 0; i < 4; ++i) {
            size_t b = len > 8 * i ? std::min<size_t>(len - 8 * i, 8) : 0;
            u64 w;
            memcpy(&w, s + 8 * i, 8);
            k.w[i] = (w | 0x2020202020202020ull) & (b == 8 ? ~0ull : (1ull << (8 * b)) - 1);
        }
        const u64 h = hash_key(k);
        terms.prefetch(h);
        const size_t slot = n++ % LAG;
        if (n > LAG) emit(terms.find(keys[slot], hashes[slot]));
        keys[slot] = k;
        hashes[slot] = h;
    }
    for (size_t i = n > LAG ? n - LAG : 0; i < n; ++i) emit(terms.find(keys[i % LAG], hashes[i % LAG]));
}

// One query, as views into the flat arrays below.
struct Query {
    bool phrase;
    std::span<const u32> terms;  // term ids
    std::span<const char> ops;   // BOOL only: 'A', 'O' or 'N' between terms
};

// All queries, flat: query i's terms are terms[first[i], first[i + 1]), and
// for BOOL ops[first[i] + k] joins its terms k and k + 1.
struct Queries {
    std::vector<u32> terms, first{0};
    std::vector<char> ops;
    std::vector<bool> phrase;

    size_t size() const { return phrase.size(); }
    Query operator[](size_t i) const {
        size_t b = first[i], n = first[i + 1] - b;
        return {phrase[i], {terms.data() + b, n}, {ops.data() + b, n - 1}};
    }
};

Key make_key(const uint8_t* p, size_t len) {
    Key k{{0, 0, 0, 0}};
    memcpy(k.w, p, std::min<size_t>(len, MAX_TERM));
    return k;
}

Queries parse_queries(const File& f, TermTable& terms) {
    Queries qs;
    const uint8_t* p = f.data;
    const uint8_t* end = f.data + f.size;
    while (p < end) {
        const uint8_t* nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
        if (!nl) nl = end;
        const bool phrase = p[0] == 'P';
        const uint8_t* s = p + (phrase ? 8 : 5);  // past `PHRASE "` or `BOOL `
        const uint8_t* e = phrase ? nl - 1 : nl;  // drop the closing quote
        for (int word = 0; s < e; ++word) {
            const uint8_t* sp = static_cast<const uint8_t*>(memchr(s, ' ', e - s));
            if (!sp) sp = e;
            if (!phrase && word % 2 == 1) {
                qs.ops.back() = static_cast<char>(*s);  // the slot of the term before it
            } else {
                qs.terms.push_back(terms.insert(make_key(s, sp - s)));
                qs.ops.push_back(0);
            }
            s = sp + 1;
        }
        qs.phrase.push_back(phrase);
        qs.first.push_back(qs.terms.size());
        p = nl + 1;
    }
    return qs;
}

// Doc-level postings for every query term. Terms found in more than 1/32 of
// the documents are bitmaps (two thirds of all postings sit in a few hundred
// such terms); the rest are sorted doc id lists in CSR form.
struct Index {
    u32 docs = 0;
    size_t words = 0;             // u64 words per bitmap
    std::vector<u32> dense;       // per term: its bitmap number, or NONE for a list
    std::vector<u64> bits;        // the bitmaps, `words` each
    std::vector<u32> df;          // per term: number of documents containing it
    // A list term's documents are stored per thread, in thread order: thread t's
    // segment holds doc - first[t] as 16 bits (a thread covers far fewer than
    // 65,536 documents), so each posting takes two bytes.
    int threads = 0;
    std::vector<u32> first;            // per thread: its first doc id
    std::vector<u32> start;            // per term, offset into post; start[n] is the total
    std::vector<std::vector<u32>> seg_end;  // per thread, per term: where its segment ends
    std::unique_ptr<uint16_t[]> post;  // doc offsets, ascending within each segment
    std::vector<u32> text;             // per doc: offset of its text, then of its end (2 per doc)
};

// An append-only list of 24-bit entries kept in blocks mapped straight from
// the kernel (pages count only once written). drain() unmaps each block as soon
// as it has been read, so the memory really goes back; malloc would keep it.
class BlockList {
public:
    static constexpr size_t BLOCK = 1 << 18;          // entries per block
    static constexpr size_t BYTES = 3 * BLOCK + 1;    // a 4-byte store at the last entry spills one byte
    ~BlockList() { for (uint8_t* b : blocks_) munmap(b, BYTES); }

    void push(u32 x) {
        if (fill_ == BLOCK) {
            cur_ = static_cast<uint8_t*>(mmap(nullptr, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            blocks_.push_back(cur_);
            fill_ = 0;
        }
        memcpy(cur_ + 3 * fill_++, &x, 4);
    }

    template <class F>
    void drain(F&& f) {
        for (size_t b = 0; b < blocks_.size(); ++b) {
            size_t n = b + 1 == blocks_.size() ? fill_ : BLOCK;
            for (size_t i = 0; i < n; ++i) {
                u32 x;
                memcpy(&x, blocks_[b] + 3 * i, 4);
                f(x & 0xFFFFFF);
            }
            munmap(blocks_[b], BYTES);
        }
        blocks_.clear();
    }

private:
    std::vector<uint8_t*> blocks_;
    uint8_t* cur_ = nullptr;
    size_t fill_ = BLOCK;
};

// The scan files each posting under a bucket of 512 consecutive term ids, as
// the term's offset in the bucket (9 bits) above the doc's offset in the
// thread's range (15 bits). The scatter then fills the posting lists one
// bucket at a time, so the lists' pages are touched only as the scan output
// that fills them is freed.
constexpr u32 BUCKET_BITS = 9, DOC_BITS = 15;

u32 parse_id(const uint8_t* p) {
    u32 v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

Index build_index(const File& f, TermTable& terms, std::vector<u32>& query_terms, int nthreads) {
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
    ix.threads = nthreads;
    ix.first.resize(nthreads);
    ix.text.resize(2 * size_t(ix.docs));
    ix.words = (ix.docs + 63) / 64;
    ix.dense.assign(nterms, NONE);
    const u32 nbuckets = (nterms >> BUCKET_BITS) + 1;

    // Each thread takes a byte range snapped to line starts. It first samples
    // the start of its range so the dense terms can be picked, then scans it all:
    // dense terms set bits, other terms' (term, doc) pairs go to `seen`, one
    // block list per bucket of terms.
    std::vector<size_t> cut(nthreads + 1);
    for (int t = 0; t <= nthreads; ++t) {
        size_t c = size * t / nthreads;
        while (c > 0 && c < size && base[c - 1] != '\n') ++c;
        cut[t] = c;
    }
    std::vector<std::vector<BlockList>> seen(nthreads);
    std::vector<std::vector<u32>> count(nthreads);
    std::vector<u32> sampled(nthreads);
    // After the sample: renumber the terms by sampled frequency, most frequent
    // first (the scan's per-term state for common terms then shares a few
    // cache lines), and make the terms over 1/32 of the sample bitmaps.
    std::barrier sync(nthreads, [&]() noexcept {
        u64 docs = 0;
        for (u32 s : sampled) docs += s;
        std::vector<u64> df(nterms);
        std::vector<u32> order(nterms), new_id(nterms);
        for (u32 id = 0; id < nterms; ++id) {
            for (int t = 0; t < nthreads; ++t) df[id] += count[t][id];
            order[id] = id;
        }
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return df[a] > df[b]; });
        u32 ndense = 0;
        for (u32 rank = 0; rank < nterms; ++rank) {
            new_id[order[rank]] = rank;
            if (df[order[rank]] * 32 > docs) ix.dense[rank] = ndense++;
        }
        terms.renumber(new_id);
        for (u32& id : query_terms) id = new_id[id];
        ix.bits.assign(ndense * ix.words, 0);
    });
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            const uint8_t* begin = base + cut[t];
            const uint8_t* end = base + cut[t + 1];
            const u32 first = ix.first[t] = begin < end ? parse_id(begin) : 0;
            auto& cnt = count[t];
            cnt.assign(nterms, 0);
            std::vector<u32> last_doc(nterms, NONE);

            // Calls term(id, doc) once per distinct known term of each document
            // in [begin, stop), and done() after each document. Returns the count.
            auto walk = [&](const uint8_t* stop, auto&& term, auto&& done) {
                u32 doc = first;
                for (const uint8_t* p = begin; p < stop; ++doc) {
                    const uint8_t* nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
                    const uint8_t* tx = static_cast<const uint8_t*>(memchr(p, '\t', nl - p)) + 1;
                    ix.text[2 * size_t(doc)] = tx - base;
                    ix.text[2 * size_t(doc) + 1] = nl - base;
                    for_each_token(tx, nl, terms, [&](u32 id) {
                        if (id != NONE && last_doc[id] != doc) last_doc[id] = doc, term(id, doc);
                    });
                    done();
                    p = nl + 1;
                }
                return doc - first;
            };

            sampled[t] = walk(begin + (end - begin) / 32, [&](u32 id, u32) { ++cnt[id]; }, [] {});
            sync.arrive_and_wait();
            std::fill(cnt.begin(), cnt.end(), 0);
            std::fill(last_doc.begin(), last_doc.end(), NONE);
            auto& out = seen[t];
            out.resize(nbuckets);
            const u32 docs = walk(end, [&](u32 id, u32 doc) {
                if (u32 d = ix.dense[id]; d != NONE) {
                    // Neighbouring threads can share a boundary word.
                    std::atomic_ref<u64>(ix.bits[d * ix.words + doc / 64])
                        .fetch_or(1ull << (doc % 64), std::memory_order_relaxed);
                } else {
                    out[id >> BUCKET_BITS].push((id & ((1u << BUCKET_BITS) - 1)) << DOC_BITS | (doc - first));
                    ++cnt[id];
                }
            }, [] {});
            if (docs >> DOC_BITS) { fputs("too many documents per thread\n", stderr); _exit(1); }
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
    ix.post.reset(new uint16_t[total]);  // not zeroed: every slot is written below
    // Each thread drains its own lists bucket by bucket into its own segment of
    // every term, so the threads never share a cursor. The cursors end at the
    // segment ends, which the queries use to decode the lists.
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            for (u32 b = 0; b < nbuckets; ++b)
                seen[t][b].drain([&](u32 x) {
                    u32 id = b << BUCKET_BITS | x >> DOC_BITS;
                    ix.post[count[t][id]++] = x & ((1u << DOC_BITS) - 1);
                });
        });
    }
    for (auto& th : pool) th.join();
    ix.seg_end = std::move(count);

    ix.df.resize(nterms);
    for (u32 id = 0; id < nterms; ++id) {
        ix.df[id] = ix.start[id + 1] - ix.start[id];
        if (ix.dense[id] != NONE)
            for (size_t w = 0; w < ix.words; ++w)
                ix.df[id] += __builtin_popcountll(ix.bits[ix.dense[id] * ix.words + w]);
    }
    return ix;
}

// One term's documents: a bitmap when `bits` is set, else the sorted list p[0, n).
struct Postings {
    const u32* p;
    size_t n;
    const u64* bits;
    size_t words;
};

inline bool has(const u64* bits, u32 doc) { return bits[doc / 64] >> (doc % 64) & 1; }

template <class F>
void for_each_bit(const u64* bits, size_t words, F&& f) {
    for (size_t w = 0; w < words; ++w)
        for (u64 m = bits[w]; m; m &= m - 1) f(u32(w * 64 + __builtin_ctzll(m)));
}

void assign(std::vector<u32>& out, Postings b) {
    out.clear();
    if (b.bits) for_each_bit(b.bits, b.words, [&](u32 x) { out.push_back(x); });
    else out.assign(b.p, b.p + b.n);
}

// First index i >= lo with a[i] >= x, searching outward from lo.
inline size_t gallop(const u32* a, size_t lo, size_t n, u32 x) {
    size_t step = 1, hi = lo;
    while (hi < n && a[hi] < x) lo = hi + 1, hi += step, step <<= 1;
    return std::lower_bound(a + lo, a + std::min(hi, n), x) - a;
}

// Set operations of a sorted array with a term's postings. Against a bitmap,
// AND and NOT are a bit test per element. Against a list, merging is linear,
// and when one side is far smaller we gallop through the larger one instead.
void op_and(const std::vector<u32>& a, Postings b, std::vector<u32>& out) {
    out.clear();
    if (b.bits) {
        for (u32 x : a)
            if (has(b.bits, x)) out.push_back(x);
    } else if (a.size() * 16 < b.n) {
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

void op_not(const std::vector<u32>& a, Postings b, std::vector<u32>& out) {
    out.clear();
    if (b.bits) {
        for (u32 x : a)
            if (!has(b.bits, x)) out.push_back(x);
    } else if (a.size() * 16 < b.n) {
        size_t j = 0;
        for (u32 x : a) {
            j = gallop(b.p, j, b.n, x);
            if (j == b.n || b.p[j] != x) out.push_back(x);
        }
    } else {
        std::set_difference(a.begin(), a.end(), b.p, b.p + b.n, std::back_inserter(out));
    }
}

void op_or(const std::vector<u32>& a, Postings b, std::vector<u32>& out) {
    out.clear();
    std::set_union(a.begin(), a.end(), b.p, b.p + b.n, std::back_inserter(out));
}

// Sum of the positions of the set bits of m: bit k of a position contributes
// 2^k for every set bit whose position has bit k set.
inline u64 position_sum(u64 m) {
    static constexpr u64 SEL[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                   0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
    u64 s = 0;
    for (int k = 0; k < 6; ++k) s += u64(__builtin_popcountll(m & SEL[k])) << k;
    return s;
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
    // A bitmap term's bitmap, or a list term's documents decoded into list_
    // (valid until the next call).
    Postings postings(u32 id) {
        if (u32 d = ix_.dense[id]; d != NONE) return {nullptr, ix_.df[id], &ix_.bits[d * ix_.words], ix_.words};
        list_.resize(ix_.df[id]);
        u32 i = ix_.start[id], n = 0;
        for (int t = 0; t < ix_.threads; ++t)
            for (u32 e = ix_.seg_end[t][id]; i < e; ++i) list_[n++] = ix_.first[t] + ix_.post[i];
        return {list_.data(), n, nullptr, ix_.words};
    }

    // The running result is a sorted list (cur_) or, once a bitmap term has been
    // OR-ed in or started the query, a bitmap (bm_), whichever stays cheaper.
    void boolean(const Query& q) {
        Postings first = postings(q.terms[0]);
        dense_ = first.bits != nullptr;
        if (dense_) bm_.assign(first.bits, first.bits + first.words);
        else cur_.assign(first.p, first.p + first.n);
        for (size_t i = 0; i < q.ops.size(); ++i) {
            Postings b = postings(q.terms[i + 1]);
            if (dense_) apply_dense(q.ops[i], b);
            else if (b.bits && q.ops[i] == 'O') {
                bm_.assign(b.bits, b.bits + b.words);
                for (u32 x : cur_) bm_[x / 64] |= 1ull << (x % 64);
                dense_ = true;
            } else {
                switch (q.ops[i]) {
                    case 'A': op_and(cur_, b, tmp_); break;
                    case 'O': op_or(cur_, b, tmp_); break;
                    default: op_not(cur_, b, tmp_); break;
                }
                cur_.swap(tmp_);
            }
        }
    }

    void apply_dense(char op, Postings b) {
        if (b.bits) {
            const size_t n = bm_.size();
            if (op == 'A') for (size_t w = 0; w < n; ++w) bm_[w] &= b.bits[w];
            else if (op == 'O') for (size_t w = 0; w < n; ++w) bm_[w] |= b.bits[w];
            else for (size_t w = 0; w < n; ++w) bm_[w] &= ~b.bits[w];
        } else if (op == 'A') {
            // Never larger than the list: back to list form.
            cur_.clear();
            for (size_t i = 0; i < b.n; ++i)
                if (has(bm_.data(), b.p[i])) cur_.push_back(b.p[i]);
            dense_ = false;
        } else {
            for (size_t i = 0; i < b.n; ++i) {
                u64 bit = 1ull << (b.p[i] % 64);
                if (op == 'O') bm_[b.p[i] / 64] |= bit;
                else bm_[b.p[i] / 64] &= ~bit;
            }
        }
    }

    void phrase(const Query& q) {
        dense_ = false;
        // Candidates hold every phrase term; intersect rarest first.
        order_.assign(q.terms.begin(), q.terms.end());
        std::sort(order_.begin(), order_.end());
        order_.erase(std::unique(order_.begin(), order_.end()), order_.end());
        std::sort(order_.begin(), order_.end(),
                  [&](u32 a, u32 b) { return ix_.df[a] < ix_.df[b]; });
        assign(cur_, postings(order_[0]));
        for (size_t i = 1; i < order_.size() && !cur_.empty(); ++i) {
            op_and(cur_, postings(order_[i]), tmp_);
            cur_.swap(tmp_);
        }
        // Keep the candidates whose text contains the phrase. Only the rarest
        // term is searched for; its neighbours are checked where it occurs.
        const size_t j = std::find(q.terms.begin(), q.terms.end(), order_[0]) - q.terms.begin();
        size_t kept = 0;
        for (u32 doc : cur_) {
            const uint8_t* b = docs_.data + ix_.text[2 * size_t(doc)];
            const uint8_t* e = docs_.data + ix_.text[2 * size_t(doc) + 1];
            if (contains(b, e, q.terms, j)) cur_[kept++] = doc;
        }
        cur_.resize(kept);
    }

    // Is the token text [p, p + len) the term `id`? Compared byte by byte, so
    // phrase checks never hash.
    bool is_term(const uint8_t* p, size_t len, u32 id) const {
        if (len != terms_.length(id)) return false;
        const uint8_t* k = terms_.bytes(id);
        for (size_t i = 0; i < len; ++i)
            if (LOWER.t[p[i]] != k[i]) return false;
        return true;
    }

    // Finds phrase[j] in [b, e) with a 32-byte scan for its first letter in either
    // case (byte | 0x20), then checks the whole token, and the phrase around it,
    // only where such a byte starts a token.
    bool contains(const uint8_t* b, const uint8_t* e, std::span<const u32> phrase, size_t j) const {
        const u32 anchor = phrase[j];
        const uint8_t first = terms_.bytes(anchor)[0];
        const __m256i want = _mm256_set1_epi8(first), case_bit = _mm256_set1_epi8(0x20);
        for (const uint8_t* p = b; p < e; p += 32) {
            u32 m = 0;
            if (e - p >= 32) {
                __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
                m = _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_or_si256(v, case_bit), want));
            } else {
                for (int i = 0; i < e - p; ++i) m |= u32((p[i] | 0x20) == first) << i;
            }
            for (; m; m &= m - 1) {
                const uint8_t* s = p + __builtin_ctz(m);
                if (s > b && LOWER.t[s[-1]]) continue;
                const uint8_t* t = s;
                while (t < e && LOWER.t[*t]) ++t;
                if (is_term(s, t - s, anchor) && around(b, e, s, t, phrase, j)) return true;
            }
        }
        return false;
    }

    // With phrase[j] at text [s, t), do the j tokens before it and the tokens
    // after it spell the rest of the phrase?
    bool around(const uint8_t* b, const uint8_t* e, const uint8_t* s, const uint8_t* t,
                std::span<const u32> phrase, size_t j) const {
        for (size_t i = j; i-- > 0;) {
            while (s > b && !LOWER.t[s[-1]]) --s;
            const uint8_t* end = s;
            while (s > b && LOWER.t[s[-1]]) --s;
            if (s == end || !is_term(s, end - s, phrase[i])) return false;
        }
        for (size_t i = j + 1; i < phrase.size(); ++i) {
            while (t < e && !LOWER.t[*t]) ++t;
            const uint8_t* start = t;
            while (t < e && LOWER.t[*t]) ++t;
            if (t == start || !is_term(start, t - start, phrase[i])) return false;
        }
        return true;
    }

    int format(char* out) const {
        u64 count = 0, sum = 0;
        u32 smallest[10];
        size_t m = 0;
        if (dense_) {
            for (size_t w = 0; w < bm_.size(); ++w) {
                u64 bits = bm_[w];
                u64 c = __builtin_popcountll(bits);
                count += c;
                sum += c * 64 * w + position_sum(bits);
                for (; bits && m < 10; bits &= bits - 1) smallest[m++] = w * 64 + __builtin_ctzll(bits);
            }
        } else {
            for (u32 x : cur_) sum += x;
            count = cur_.size();
            for (; m < count && m < 10; ++m) smallest[m] = cur_[m];
        }
        char* p = put(out, count);
        *p++ = ' ';
        p = put(p, sum);
        for (size_t i = 0; i < m; ++i) {
            *p++ = ' ';
            p = put(p, smallest[i]);
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
    std::vector<u32> cur_, tmp_, order_, list_;
    std::vector<u64> bm_;
    bool dense_ = false;
};

}  // namespace

int main() {
    const int nthreads = std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
    File qf = map_file("/data/queries.txt");
    File df = map_file("/data/docs.txt");

    TermTable terms;
    Queries queries = parse_queries(qf, terms);
    Index ix = build_index(df, terms, queries.terms, nthreads);

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
