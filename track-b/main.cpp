// Speed Demon 2026, Track B (Index).
//
// The queries are readable up front, so we read them first and index only the
// terms they mention. One parallel pass over the memory-mapped corpus builds
// doc-level postings for those terms (bitmaps for common terms, sorted lists
// for the rest) and checks each phrase wherever its rarest term occurs. BOOL
// queries are then evaluated left to right, galloping when one side is much
// smaller. The few phrases without a rare term intersect their terms'
// postings, rarest first, and search only the surviving candidates.
#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <immintrin.h>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <utility>
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
    // Room for fewer than n keys. The caller's bound counts term occurrences,
    // about twice the distinct terms, so the load stays low.
    // calloc takes large blocks straight from the kernel: zero pages, no memset.
    explicit TermTable(size_t n)
        : slots_(static_cast<u64*>(calloc(std::bit_ceil(n), 8))), keys_(new Key[n]), lens_(new uint8_t[n]),
          mask_(std::bit_ceil(n) - 1) {}
    ~TermTable() { free(slots_); }
    u32 size() const { return size_; }
    const uint8_t* bytes(u32 id) const { return reinterpret_cast<const uint8_t*>(keys_[id].w); }
    size_t length(u32 id) const { return lens_[id]; }

    // Safe from several threads at once: an empty slot is claimed with a CAS and
    // marked BUSY until its key is stored, so every key gets exactly one id.
    u32 insert(const Key& k) {
        constexpr u32 BUSY = NONE;
        const u64 h = hash_key(k), tag = h & 0xFFFFFFFF00000000ull;
        for (size_t i = h & mask_;; i = (i + 1) & mask_) {
            std::atomic_ref<u64> slot(slots_[i]);
            u64 s = slot.load(std::memory_order_acquire);
            if (s == 0 && slot.compare_exchange_strong(s, tag | BUSY, std::memory_order_acquire)) {
                const u32 id = size_.fetch_add(1, std::memory_order_relaxed);
                keys_[id] = k;
                lens_[id] = strnlen(reinterpret_cast<const char*>(k.w), MAX_TERM);
                slot.store(tag | (id + 1), std::memory_order_release);
                return id;
            }
            if ((s & 0xFFFFFFFF00000000ull) != tag) continue;  // after a lost CAS, s is the winner's
            while (u32(s) == BUSY) s = slot.load(std::memory_order_acquire);
            if (same(keys_[u32(s) - 1], k)) return u32(s) - 1;
        }
    }

    // Old id i becomes new_id[i], under `space`; the slots are rewritten in place.
    void renumber(const std::vector<u32>& new_id, size_t space) {
        std::unique_ptr<Key[]> keys(new Key[space]);
        std::unique_ptr<uint8_t[]> lens(new uint8_t[space]);
        for (u32 i = 0; i < size_; ++i) keys[new_id[i]] = keys_[i], lens[new_id[i]] = lens_[i];
        keys_.swap(keys);
        lens_.swap(lens);
        for (size_t i = 0; i <= mask_; ++i)
            if (u64 s = slots_[i]) slots_[i] = (s & 0xFFFFFFFF00000000ull) | (new_id[u32(s) - 1] + 1);
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
    u64* slots_;
    std::unique_ptr<Key[]> keys_;
    std::unique_ptr<uint8_t[]> lens_;
    size_t mask_;
    std::atomic<u32> size_{0};
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

// Calls emit(term_id, start) for every token of text [q, e) that is a known
// term (NONE for tokens that are not; tokens longer than 32 bytes are skipped).
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
    const uint8_t* starts[LAG];
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
        if (n > LAG) emit(terms.find(keys[slot], hashes[slot]), starts[slot]);
        keys[slot] = k;
        hashes[slot] = h;
        starts[slot] = s;
    }
    for (size_t i = n > LAG ? n - LAG : 0; i < n; ++i)
        emit(terms.find(keys[i % LAG], hashes[i % LAG]), starts[i % LAG]);
}

// One query, as views into the flat arrays below.
struct Query {
    size_t index;
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
        return {i, phrase[i], {terms.data() + b, n}, {ops.data() + b, n - 1}};
    }
};

Key make_key(const uint8_t* p, size_t len) {
    Key k{{0, 0, 0, 0}};
    memcpy(k.w, p, std::min<size_t>(len, MAX_TERM));
    return k;
}

// Parses the query lines in [p, end).
void parse_lines(const uint8_t* p, const uint8_t* end, TermTable& terms, Queries& qs) {
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
}

// Threads parse line-aligned parts of the file into one shared term table,
// then the parts are joined in file order.
Queries parse_queries(const File& f, TermTable& terms, int nthreads) {
    std::vector<Queries> part(nthreads);
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            auto line_start = [&](size_t c) {
                while (c > 0 && c < f.size && f.data[c - 1] != '\n') ++c;
                return f.data + c;
            };
            parse_lines(line_start(f.size * t / nthreads), line_start(f.size * (t + 1) / nthreads), terms, part[t]);
        });
    }
    for (auto& th : pool) th.join();
    Queries qs = std::move(part[0]);
    for (int t = 1; t < nthreads; ++t) {
        const u32 base = qs.terms.size();
        qs.terms.insert(qs.terms.end(), part[t].terms.begin(), part[t].terms.end());
        qs.ops.insert(qs.ops.end(), part[t].ops.begin(), part[t].ops.end());
        qs.phrase.insert(qs.phrase.end(), part[t].phrase.begin(), part[t].phrase.end());
        for (size_t i = 1; i < part[t].first.size(); ++i) qs.first.push_back(base + part[t].first[i]);
    }
    return qs;
}

// Doc-level postings for every query term. Terms found in more than 1/32 of
// the documents are bitmaps (two thirds of all postings sit in a few hundred
// such terms); the rest are sorted doc id lists in CSR form.
struct Index {
    u32 docs = 0;
    size_t words = 0;             // u64 words per bitmap
    u32 ndense = 0;               // terms [0, ndense) are bitmaps
    u32 list_base = 0;            // list terms start here, at a bucket boundary
    u64* bits = nullptr;          // the bitmaps, `words` each, in term order
    std::vector<u32> df;          // per term: number of documents containing it
    // A list term's documents are post[start[id], start[id + 1]), one segment
    // per thread in thread order: thread t's seglen[id * threads + t] postings
    // hold doc - first[t] as 16 bits (a thread covers far fewer than 65,536
    // documents), two bytes a posting.
    int threads = 0;
    std::vector<u32> first;            // per thread: its first doc id
    std::vector<u32> start;            // per term: offset into post
    std::vector<uint16_t> seglen;      // per term and thread: segment length
    std::unique_ptr<uint16_t[]> post;  // doc offsets, ascending within each segment
    std::vector<u32> text;             // per doc: file offset of its text
    // A phrase's anchor is its rarest term, when that is rare: the scan checks
    // the phrase wherever the anchor occurs, while the text is in cache, and
    // keeps the matching documents (ascending, possibly repeated).
    u32 anchors = NONE;                // the first anchor's term id; anchors come last
    std::vector<bool> anchored;        // per query: a phrase checked by the scan
    std::vector<u32> found_start;      // per query: offset into found
    std::vector<u32> found;            // documents matching each anchored phrase
};

// Is the token text [p, p + len) the term `id`? Compared byte by byte, so
// phrase checks never hash.
bool is_term(const TermTable& terms, const uint8_t* p, size_t len, u32 id) {
    if (len != terms.length(id)) return false;
    const uint8_t* k = terms.bytes(id);
    for (size_t i = 0; i < len; ++i)
        if (LOWER.t[p[i]] != k[i]) return false;
    return true;
}

// With phrase[j] at text [s, t), do the j tokens before it and the tokens
// after it spell the rest of the phrase? The document's TAB and LF bound
// the search (the id before the TAB is never reached).
bool around(const TermTable& terms, const uint8_t* s, const uint8_t* t, std::span<const u32> phrase, size_t j) {
    for (size_t i = j; i-- > 0;) {
        for (; !LOWER.t[s[-1]]; --s)
            if (s[-1] == '\t') return false;
        const uint8_t* end = s;
        while (LOWER.t[s[-1]]) --s;
        if (!is_term(terms, s, end - s, phrase[i])) return false;
    }
    for (size_t i = j + 1; i < phrase.size(); ++i) {
        for (; !LOWER.t[*t]; ++t)
            if (*t == '\n') return false;
        const uint8_t* start = t;
        while (LOWER.t[*t]) ++t;
        if (!is_term(terms, start, t - start, phrase[i])) return false;
    }
    return true;
}

// An append-only list of 16-bit units kept in blocks mapped straight from the
// kernel (pages count only once written). drain() unmaps each block as soon as
// it has been read, so the memory really goes back; malloc would keep it.
class BlockList {
public:
    static constexpr size_t BLOCK = 1 << 18;  // units per block (512 KB)
    ~BlockList() { for (uint16_t* b : blocks_) munmap(b, 2 * BLOCK); }

    void push(uint16_t x) {
        if (fill_ == BLOCK) {
            cur_ = static_cast<uint16_t*>(mmap(nullptr, 2 * BLOCK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            blocks_.push_back(cur_);
            fill_ = 0;
        }
        cur_[fill_++] = x;
    }

    template <class F>
    void drain(F&& f) {
        for (size_t b = 0; b < blocks_.size(); ++b) {
            size_t n = b + 1 == blocks_.size() ? fill_ : BLOCK;
            for (size_t i = 0; i < n; ++i) f(blocks_[b][i]);
            munmap(blocks_[b], 2 * BLOCK);
        }
        blocks_.clear();
    }

private:
    std::vector<uint16_t*> blocks_;
    uint16_t* cur_ = nullptr;
    size_t fill_ = BLOCK;
};

// The scan files each posting under a bucket of 2,048 consecutive term ids
// (about 37 buckets, so each thread writes 37 streams; 512-term buckets meant
// 148 and ran 10% slower). A thread meets its documents in order, so within
// one bucket's stream the docs never go down: a posting is one 16-bit unit,
// the term's offset in the bucket (11 bits) above the gap since the bucket's
// previous doc (5 bits). A gap of
// GAP or more is written as GAP followed by a unit holding the whole gap (a
// thread covers fewer than 2^15 docs). The scatter then fills the posting
// lists one bucket at a time, so the lists' pages are touched only as the scan
// output that fills them is freed.
constexpr u32 BUCKET_BITS = 11, GAP_BITS = 16 - BUCKET_BITS, DOC_BITS = 15, GAP = (1u << GAP_BITS) - 1;

u32 parse_id(const uint8_t* p) {
    u32 v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

Index build_index(const File& f, TermTable& terms, Queries& qs, int nthreads) {
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
    ix.text.resize(ix.docs);
    ix.words = (ix.docs + 63) / 64;
    u32 nanchors = 0, space = nterms, nbuckets = 1;

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
    std::vector<u32> by_anchor_start;                     // per anchor: offset into by_anchor
    std::vector<std::pair<u32, u32>> by_anchor;           // (query, anchor position)
    std::vector<std::vector<std::pair<u32, u32>>> found(nthreads);  // (query, doc) matches
    std::vector<u32> sampled(nthreads), totals(nthreads);
    // After the sample: pick the phrase anchors (a phrase's rarest term in the
    // sample, if it is in under 1/200 of the documents) and number them last,
    // so the scan tells them apart by id alone. Deal the other terms out by
    // sampled frequency, most frequent first, round-robin over the buckets of
    // the scan output: every bucket then holds a similar share of the postings
    // (so the scatter never holds one large bucket twice over), and the
    // per-term state of common terms still shares a few cache lines at each
    // bucket's start. Terms over 1/32 of the sample become bitmaps.
    std::barrier sync(nthreads, [&]() noexcept {
        u64 docs = 0;
        for (u32 s : sampled) docs += s;
        std::vector<u64> df(nterms);
        for (auto& c : count)
            for (u32 id = 0; id < nterms; ++id) df[id] += c[id];
        std::vector<bool> anchor(nterms);
        std::vector<u32> where(qs.size(), NONE);  // per phrase: its anchor's position
        for (size_t i = 0; i < qs.size(); ++i) {
            if (!qs.phrase[i]) continue;
            Query q = qs[i];
            auto a = std::min_element(q.terms.begin(), q.terms.end(), [&](u32 x, u32 y) { return df[x] < df[y]; });
            if (df[*a] * 200 > docs) continue;
            where[i] = a - q.terms.begin();
            if (!anchor[*a]) anchor[*a] = true, ++nanchors;
        }
        // Most frequent first, then old id, anchors last: a counting sort, since
        // a sampled df is at most the sampled documents.
        auto rank_key = [&](u32 id) { return anchor[id] ? docs + 1 : docs - df[id]; };
        std::vector<u32> start(docs + 3), order(nterms);
        for (u32 id = 0; id < nterms; ++id) ++start[rank_key(id) + 1];
        std::partial_sum(start.begin(), start.end(), start.begin());
        for (u32 id = 0; id < nterms; ++id) order[start[rank_key(id)]++] = id;
        const u32 lists = nterms - nanchors, bucket = 1u << BUCKET_BITS;
        u32& ndense = ix.ndense;
        while (ndense < lists && df[order[ndense]] * 32 > docs) ++ndense;
        ix.list_base = (ndense + bucket - 1) & ~(bucket - 1);
        const u32 dealt = std::max(1u, (lists - ndense + bucket - 1) >> BUCKET_BITS);  // buckets of non-anchors
        ix.anchors = ix.list_base + (dealt << BUCKET_BITS);
        space = ix.anchors + nanchors;
        nbuckets = dealt + ((nanchors + bucket - 1) >> BUCKET_BITS);  // anchors keep lists too, for BOOL
        std::vector<u32> new_id(nterms);
        for (u32 rank = 0; rank < nterms; ++rank) {
            const u32 r = rank - ndense;
            new_id[order[rank]] = rank < ndense  ? rank
                                  : rank < lists ? ix.list_base + ((r % dealt) << BUCKET_BITS | r / dealt)
                                                 : ix.anchors + rank - lists;
        }
        terms.renumber(new_id, space);
        for (u32& id : qs.terms) id = new_id[id];
        // Per anchor, its phrases (query, anchor position), for the scan.
        ix.anchored.resize(qs.size());
        by_anchor_start.assign(nanchors + 2, 0);
        for (size_t i = 0; i < qs.size(); ++i)
            if (where[i] != NONE) ix.anchored[i] = true, ++by_anchor_start[qs[i].terms[where[i]] - ix.anchors + 2];
        std::partial_sum(by_anchor_start.begin(), by_anchor_start.end(), by_anchor_start.begin());
        by_anchor.resize(by_anchor_start.back());
        for (size_t i = 0; i < qs.size(); ++i)
            if (where[i] != NONE) by_anchor[by_anchor_start[qs[i].terms[where[i]] - ix.anchors + 1]++] = {u32(i), where[i]};
        // Straight from the kernel: zero pages, touched only as the scan sets bits.
        ix.bits = static_cast<u64*>(mmap(nullptr, std::max<size_t>(ndense * ix.words, 1) * 8, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
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

            // Calls f(id, start, doc) for every token of each document in
            // [begin, stop). Returns the document count.
            auto walk = [&](const uint8_t* stop, auto&& f) {
                u32 doc = first;
                for (const uint8_t* p = begin; p < stop; ++doc) {
                    const uint8_t* nl = static_cast<const uint8_t*>(memchr(p, '\n', end - p));
                    const uint8_t* tx = static_cast<const uint8_t*>(memchr(p, '\t', nl - p)) + 1;
                    ix.text[doc] = tx - base;
                    for_each_token(tx, nl, terms, [&](u32 id, const uint8_t* s) { f(id, s, doc); });
                    p = nl + 1;
                }
                return doc - first;
            };

            sampled[t] = walk(begin + (end - begin) / 32, [&](u32 id, const uint8_t*, u32 doc) {
                if (id != NONE && last_doc[id] != doc) last_doc[id] = doc, ++cnt[id];
            });
            sync.arrive_and_wait();
            cnt.assign(space, 0);  // list postings per term
            last_doc.assign(space, NONE);
            auto& out = seen[t];
            out.resize(nbuckets);
            std::vector<uint16_t> prev(nbuckets);  // per bucket: offset of its last doc
            // A bitmap term collects its bits for the current 64 documents here
            // and ORs them into its bitmap when it moves on (atomically, since
            // neighbouring threads can share a boundary word).
            struct Pending {
                u64 bits = 0;
                u32 word = 0;
            };
            std::vector<Pending> pend(ix.ndense);
            auto flush = [&](u32 id) {
                if (pend[id].bits)
                    std::atomic_ref<u64>(ix.bits[id * ix.words + pend[id].word])
                        .fetch_or(std::exchange(pend[id].bits, 0), std::memory_order_relaxed);
            };
            const u32 docs = walk(end, [&](u32 id, const uint8_t* s, u32 doc) {
                if (id < ix.ndense) {
                    if (pend[id].word != doc / 64) flush(id), pend[id].word = doc / 64;
                    pend[id].bits |= 1ull << (doc % 64);
                    return;
                }
                if (id == NONE) return;
                if (id >= ix.anchors) {
                    const u32 a = id - ix.anchors;
                    for (u32 k = by_anchor_start[a]; k < by_anchor_start[a + 1]; ++k)
                        if (auto [i, j] = by_anchor[k]; around(terms, s, s + terms.length(id), qs[i].terms, j))
                            found[t].push_back({i, doc});
                }
                if (last_doc[id] == doc) return;
                last_doc[id] = doc;
                u32 b = (id - ix.list_base) >> BUCKET_BITS, off = doc - first, gap = off - prev[b];
                prev[b] = off;
                out[b].push((id & ((1u << BUCKET_BITS) - 1)) << GAP_BITS | std::min(gap, GAP));
                if (gap >= GAP) out[b].push(gap);
                ++cnt[id];
            });
            for (u32 id = 0; id < ix.ndense; ++id) flush(id);
            if (docs >> DOC_BITS) { fputs("too many documents per thread\n", stderr); _exit(1); }
            totals[t] = std::accumulate(cnt.begin(), cnt.end(), 0u);
        });
    }
    for (auto& th : pool) th.join();
    pool.clear();

    // The phrase matches, per query; threads cover the documents in order.
    ix.found_start.assign(qs.size() + 1, 0);
    for (auto& f : found)
        for (auto [i, doc] : f) ++ix.found_start[i + 1];
    std::partial_sum(ix.found_start.begin(), ix.found_start.end(), ix.found_start.begin());
    ix.found.resize(ix.found_start.back());
    std::vector<u32> fill(ix.found_start.begin(), ix.found_start.end() - 1);
    for (auto& f : found)
        for (auto [i, doc] : f) ix.found[fill[i]++] = doc;

    ix.post.reset(new uint16_t[std::accumulate(totals.begin(), totals.end(), 0u)]);  // every slot is written below
    ix.start.resize(space + 1);
    ix.seglen.resize(size_t(space) * nthreads);
    // Thread t lays out the lists of the term ids [lo, hi): it sums their
    // postings, waits for every slice's sum, then sets each term's start, its
    // segment lengths and every thread's write cursor there (count[s][id]).
    // Once all are laid out, each thread scatters its own scan output, bucket
    // by bucket; a bucket's terms are consecutive ids, so its lists are one
    // stretch of post.
    std::vector<u32> slice(nthreads + 1);
    std::barrier summed(nthreads, [&]() noexcept { std::exclusive_scan(slice.begin(), slice.end(), slice.begin(), 0u); });
    std::barrier laid(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&, t] {
            const u32 lo = u64(space) * t / nthreads, hi = u64(space) * (t + 1) / nthreads;
            for (int s = 0; s < nthreads; ++s)
                slice[t] = std::accumulate(count[s].begin() + lo, count[s].begin() + hi, slice[t]);
            summed.arrive_and_wait();
            u32 pos = slice[t];
            for (u32 id = lo; id < hi; ++id) {
                ix.start[id] = pos;
                for (int s = 0; s < nthreads; ++s) {
                    ix.seglen[size_t(id) * nthreads + s] = count[s][id];
                    pos += std::exchange(count[s][id], pos);
                }
            }
            if (t == nthreads - 1) ix.start[space] = pos;
            laid.arrive_and_wait();
            for (u32 b = 0; b < nbuckets; ++b) {
                u32 doc = 0, id = 0;
                bool wide = false;  // the next unit is a whole gap
                seen[t][b].drain([&](uint16_t x) {
                    if (!wide) {
                        id = ix.list_base + (b << BUCKET_BITS | x >> GAP_BITS);
                        if ((x & GAP) == GAP) { wide = true; return; }
                        doc += x & GAP;
                    } else {
                        doc += x, wide = false;
                    }
                    ix.post[count[t][id]++] = doc;
                });
            }
        });
    }
    for (auto& th : pool) th.join();
    std::vector<std::vector<u32>>().swap(count);

    // Documents per term, to order a phrase's terms (a bitmap counts as all).
    ix.df.resize(space);
    for (u32 id = 0; id < space; ++id) {
        ix.df[id] = id < ix.ndense ? ix.docs : ix.start[id + 1] - ix.start[id];
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
        if (id < ix_.ndense) return {nullptr, ix_.df[id], &ix_.bits[id * ix_.words], ix_.words};
        list_.resize(ix_.df[id]);
        u32 n = 0;
        const uint16_t* len = &ix_.seglen[size_t(id) * ix_.threads];
        for (u32 t = 0, i = ix_.start[id]; t < u32(ix_.threads); ++t)
            for (const u32 e = i + len[t], first = ix_.first[t]; i < e; ++i) list_[n++] = first + ix_.post[i];
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
        cur_.clear();
        if (ix_.anchored[q.index]) {  // checked by the scan
            for (u32 k = ix_.found_start[q.index]; k < ix_.found_start[q.index + 1]; ++k)
                if (cur_.empty() || cur_.back() != ix_.found[k]) cur_.push_back(ix_.found[k]);
            return;
        }
        // Otherwise candidates hold every phrase term; intersect rarest first.
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
            const uint8_t* b = docs_.data + ix_.text[doc];
            const uint8_t* e = static_cast<const uint8_t*>(memchr(b, '\n', docs_.size - (b - docs_.data)));
            if (contains(b, e, q.terms, j)) cur_[kept++] = doc;
        }
        cur_.resize(kept);
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
                if (is_term(terms_, s, t - s, anchor) && around(terms_, s, t, phrase, j)) return true;
            }
        }
        return false;
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
    // With transparent huge pages, a block written only in part can be backed
    // by a whole 2 MB page; plain pages keep memory to what is written.
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    // Large blocks always come from mmap and go back to the kernel when freed
    // (glibc otherwise raises this threshold after the first free and keeps them).
    mallopt(M_MMAP_THRESHOLD, 1 << 16);
    const int nthreads = std::max(1u, std::min(12u, std::thread::hardware_concurrency()));
    File qf = map_file("/data/queries.txt");
    File df = map_file("/data/docs.txt");

    // Every term follows a space or a quote, and every quote-led term follows
    // a space before its quote; a space before an operator starts no term.
    size_t occurrences = 1;
    for (size_t i = 0; i + 1 < qf.size; ++i) occurrences += qf.data[i] == ' ' && (qf.data[i + 1] < 'A' || qf.data[i + 1] > 'Z');
    TermTable terms(occurrences);
    Queries queries = parse_queries(qf, terms, nthreads);
    Index ix = build_index(df, terms, queries, nthreads);

    // Answer queries in parallel, CHUNK at a time; each chunk's lines go to its
    // own buffer (a line is at most 6 + 12 + 10 * 7 digits and spaces).
    constexpr size_t CHUNK = 64;
    const size_t nq = queries.size(), nchunks = (nq + CHUNK - 1) / CHUNK;
    std::vector<std::string> chunks(nchunks);
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) {
        pool.emplace_back([&] {
            Solver solver(ix, terms, df);
            char line[128];
            for (size_t c; (c = next++) < nchunks;)
                for (size_t j = c * CHUNK; j < std::min(nq, (c + 1) * CHUNK); ++j)
                    chunks[c].append(line, solver.answer(queries[j], line));
        });
    }
    for (auto& th : pool) th.join();

    FILE* f = fopen("/out/results.txt", "wb");
    if (!f) { perror("/out/results.txt"); return 1; }
    for (const std::string& c : chunks) fwrite(c.data(), 1, c.size(), f);
    fclose(f);
    return 0;
}
