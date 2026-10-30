// Speed Demon 2026, Track D (Traverse).
//
// Node ids in graph.txt are a random relabelling, so a search over them would
// miss the cache on almost every step. The memory-mapped text is parsed once
// (line-aligned ranges, one per thread), each thread filing its edges by tail
// range; each owner of a range then builds its slice of the CSR arrays. After
// the nodes are renumbered, the CSR is copied into the new numbering.
// The renumbering cuts the road network into compact blobs of about 512 nodes:
// each thread grows a blob by BFS over unclaimed nodes, then starts the next
// one from the blob's frontier, so consecutive ids are close on the map and a
// search's ball covers a few blobs densely. Self-loops are dropped (they never
// shorten a path).
//
// Queries run on all threads, one Dijkstra search each with a radix heap and
// early exit; a target with no in-edge is answered -1 without searching. Each
// thread keeps its distances in 4 KB pages, allocated on first touch and
// recycled after every query, so its memory follows the largest search
// instead of the node count.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u64 INF = ~u64(0);

// Map a file over a spare zero page, so reads just past its end are safe.
const char* map_file(const char* path, size_t& size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); _exit(1); }
    size = lseek(fd, 0, SEEK_END);
    auto* p = static_cast<char*>(mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (size && mmap(p, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        perror("mmap");
        _exit(1);
    }
    close(fd);
    return p;
}

// Digits up to the next non-digit; p is left just past the separator after them.
u32 number(const char*& p) {
    u32 v = *p++ - '0';
    while (u32(*p - '0') < 10) v = v * 10 + (*p++ - '0');
    ++p;
    return v;
}

// A radix heap: Dijkstra's keys never drop below the last one popped, so an
// entry lives in the bucket of the highest bit where it differs from that key
// and only the lowest non-empty bucket is ever split.
class RadixHeap {
public:
    bool empty() const { return size_ == 0; }

    void clear() {
        for (auto& b : buckets_) b.clear();
        size_ = 0;
        last_ = 0;
    }

    void push(u64 key, u32 node) {
        buckets_[slot(key)].push_back({key, node});
        ++size_;
    }

    std::pair<u64, u32> pop() {
        if (buckets_[0].empty()) {
            int i = 1;
            while (buckets_[i].empty()) ++i;
            last_ = std::min_element(buckets_[i].begin(), buckets_[i].end())->first;
            for (auto e : buckets_[i]) buckets_[slot(e.first)].push_back(e);
            buckets_[i].clear();
        }
        --size_;
        auto e = buckets_[0].back();
        buckets_[0].pop_back();
        return e;
    }

private:
    int slot(u64 key) const { return key == last_ ? 0 : 64 - __builtin_clzll(key ^ last_); }

    std::vector<std::pair<u64, u32>> buckets_[65];
    size_t size_ = 0;
    u64 last_ = 0;
};

// One search's tentative distances, INF where unset, in 4 KB pages that are
// handed out on first touch and taken back by reset().
class Distances {
public:
    static constexpr u32 PAGE = 512;

    explicit Distances(u32 n) : table_((n + PAGE - 1) / PAGE, nullptr) {}

    u64& operator[](u32 v) {
        u64*& page = table_[v / PAGE];
        if (!page) {
            if (spare_.empty()) owned_.emplace_back(new u64[PAGE]), spare_.push_back(owned_.back().get());
            page = spare_.back();
            spare_.pop_back();
            std::fill(page, page + PAGE, INF);
            used_.push_back(v / PAGE);
        }
        return page[v % PAGE];
    }

    void reset() {
        for (u32 i : used_) spare_.push_back(table_[i]), table_[i] = nullptr;
        used_.clear();
    }

private:
    std::vector<u64*> table_, spare_;
    std::vector<u32> used_;
    std::vector<std::unique_ptr<u64[]>> owned_;
};

template <class F>
void run_threads(int n, F&& body) {
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t) pool.emplace_back(body, t);
    for (auto& th : pool) th.join();
}

int main() {
    // Large blocks always come from mmap and go back to the kernel when freed
    // (glibc otherwise raises this threshold after the first free and keeps them).
    mallopt(M_MMAP_THRESHOLD, 1 << 16);
    const int nthreads = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
    size_t gsize, qsize;
    const char* graph = map_file("/data/graph.txt", gsize);
    const char* qtext = map_file("/data/queries.txt", qsize);

    std::vector<std::pair<u32, u32>> queries;
    for (const char* p = qtext; p < qtext + qsize;) {
        u32 s = number(p);
        queries.emplace_back(s, number(p));
    }

    const char* p = graph;
    const u32 n = number(p);
    number(p);  // m: the edge lines themselves are counted
    std::vector<size_t> cut(nthreads + 1, gsize);
    cut[0] = p - graph;
    for (int t = 1; t < nthreads; ++t) {
        size_t c = std::max(cut[t - 1], cut[0] + (gsize - cut[0]) * t / nthreads);
        while (c < gsize && graph[c - 1] != '\n') ++c;
        cut[t] = c;
    }
    // Calls edge(u, v, w) for every edge line in thread t's range but self-loops.
    auto parse = [&](int t, auto&& edge) {
        for (const char* q = graph + cut[t]; q < graph + cut[t + 1];) {
            u32 u = number(q), v = number(q), w = number(q);
            if (u != v) edge(u, v, w);
        }
    };

    // Only query targets need an in-edge check; flag them before the parse.
    std::vector<std::atomic<u8>> target(n);  // 1 = queried target, 2 = also has an in-edge
    for (auto [s, t] : queries) target[t].store(1, std::memory_order_relaxed);

    // One parse: each thread files its edges by tail range, one owner per
    // thread. Then each owner counts and places the edges of its own nodes into
    // its own contiguous slice of the CSR arrays: no atomics, local writes.
    struct Line {
        u32 u, v, w;
    };
    const u32 span = (n + nthreads - 1) / nthreads;  // nodes per owner
    std::vector<std::vector<std::vector<Line>>> filed(nthreads, std::vector<std::vector<Line>>(nthreads));
    run_threads(nthreads, [&](int t) {
        for (auto& f : filed[t]) f.reserve((cut[t + 1] - cut[t]) / 18 / nthreads * 9 / 8);
        parse(t, [&](u32 u, u32 v, u32 w) {
            filed[t][u / span].push_back({u, v, w});
            if (target[v].load(std::memory_order_relaxed)) target[v].store(2, std::memory_order_relaxed);
        });
    });
    std::vector<u32> base(nthreads + 1);  // first edge of each owner's slice
    for (int o = 0; o < nthreads; ++o) {
        base[o + 1] = base[o];
        for (int t = 0; t < nthreads; ++t) base[o + 1] += filed[t][o].size();
    }
    std::vector<u32> start(n + 1), heads(base[nthreads]), weights(base[nthreads]);
    start[n] = base[nthreads];
    run_threads(nthreads, [&](int o) {
        const u32 lo = std::min<u64>(n, u64(o) * span), hi = std::min<u64>(n, u64(o + 1) * span);
        std::vector<u32> next(hi - lo + 1);
        for (int t = 0; t < nthreads; ++t)
            for (const Line& l : filed[t][o]) ++next[l.u - lo + 1];
        next[0] = base[o];
        for (u32 i = 0; i < hi - lo; ++i) start[lo + i] = next[i], next[i + 1] += next[i];
        for (int t = 0; t < nthreads; ++t) {
            for (const Line& l : filed[t][o]) {
                u32 e = next[l.u - lo]++;
                heads[e] = l.v;
                weights[e] = l.w;
            }
            std::vector<Line>().swap(filed[t][o]);
        }
    });

    // Blobs. A thread grows each blob by BFS over unclaimed nodes until it holds
    // BLOB nodes; the blob's unexpanded nodes' unclaimed neighbours become the
    // seeds of the next ones, so a thread's blobs form a chain across the map.
    // Threads start far apart; nodes nobody reached (islands and the like) come
    // last. Claims are compare-exchanges, so blobs never share a node.
    constexpr size_t BLOB = 512;
    std::vector<std::atomic<u8>> claimed(n);
    auto claim = [&](u32 v) {
        u8 no = 0;
        return !claimed[v].load(std::memory_order_relaxed) && claimed[v].compare_exchange_strong(no, 1);
    };
    std::vector<std::vector<u32>> chain(nthreads);
    run_threads(nthreads, [&](int t) {
        auto& order = chain[t];
        std::vector<u32> seeds;
        u32 scan = u64(n) * t / nthreads;
        for (;;) {
            u32 seed = n;
            while (!seeds.empty() && seed == n) {
                if (claim(seeds.back())) seed = seeds.back();
                seeds.pop_back();
            }
            while (seed == n && scan < n) {
                if (start[scan + 1] > start[scan] && claim(scan)) seed = scan;
                ++scan;
            }
            if (seed == n) break;
            size_t i = order.size(), blob = i;
            order.push_back(seed);
            for (; i < order.size() && order.size() - blob < BLOB; ++i)
                for (u32 e = start[order[i]]; e < start[order[i] + 1] && order.size() - blob < BLOB; ++e)
                    if (claim(heads[e])) order.push_back(heads[e]);
            for (; i < order.size(); ++i)
                for (u32 e = start[order[i]]; e < start[order[i] + 1]; ++e)
                    if (!claimed[heads[e]].load(std::memory_order_relaxed)) seeds.push_back(heads[e]);
        }
    });
    std::vector<u32> id(n);  // old id -> new id
    u32 k = 0;
    for (auto& order : chain) {
        for (u32 u : order) id[u] = k++;
        std::vector<u32>().swap(order);
    }
    for (u32 u = 0; u < n; ++u)
        if (!claimed[u].load(std::memory_order_relaxed)) id[u] = k++;
    std::vector<std::atomic<u8>>().swap(claimed);

    // The CSR in the new numbering, copied node by node in parallel: weights
    // first, then heads, freeing each old array as soon as it is copied, so at
    // most one and a half graphs are ever resident.
    std::vector<u32> first(n + 1);
    for (u32 u = 0; u < n; ++u) first[id[u] + 1] = start[u + 1] - start[u];
    for (u32 v = 0; v < n; ++v) first[v + 1] += first[v];
    auto renumber = [&](std::vector<u32>& old, auto&& map) {
        std::vector<u32> fresh(old.size());
        run_threads(nthreads, [&](int t) {
            for (u32 u = u64(n) * t / nthreads; u < u64(n) * (t + 1) / nthreads; ++u)
                for (u32 e = start[u], j = first[id[u]]; e < start[u + 1]; ++e, ++j) fresh[j] = map(old[e]);
        });
        old.swap(fresh);
    };
    renumber(weights, [](u32 w) { return w; });
    renumber(heads, [&](u32 v) { return id[v]; });
    std::vector<u32>().swap(start);
    const std::vector<u32>&to = heads, &w = weights;

    // Queries, claimed one at a time.
    std::vector<u64> answer(queries.size());
    std::atomic<size_t> claim_query{0};
    run_threads(nthreads, [&](int) {
        Distances dist(n);
        RadixHeap heap;
        for (size_t i; (i = claim_query++) < queries.size();) {
            auto [s0, t0] = queries[i];
            answer[i] = s0 == t0 ? 0 : INF;
            if (s0 == t0 || target[t0].load(std::memory_order_relaxed) != 2) continue;
            const u32 s = id[s0], t = id[t0];
            heap.clear();
            heap.push(0, s);
            dist[s] = 0;
            while (!heap.empty()) {
                auto [d, u] = heap.pop();
                if (d > dist[u]) continue;  // stale: u was settled closer
                if (u == t) { answer[i] = d; break; }
                for (u32 e = first[u]; e < first[u + 1]; ++e) {
                    u64 nd = d + w[e];
                    u64& dv = dist[to[e]];
                    if (nd < dv) dv = nd, heap.push(nd, to[e]);
                }
            }
            dist.reset();
        }
    });

    std::string out;
    for (u64 a : answer) out += a == INF ? std::string("-1\n") : std::to_string(a) + '\n';
    FILE* f = fopen("/out/distances.txt", "wb");
    if (!f) { perror("/out/distances.txt"); return 1; }
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return 0;
}
