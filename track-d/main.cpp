// Speed Demon 2026, Track D (Traverse) — Optimized for K (Time T, Memory M, and
// Source Size S).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <malloc.h>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <vector>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
constexpr u64 INF = ~u64(0);
struct __attribute__((packed)) Item {
  u64 d;
  u32 u;
};
const char *map_file(const char *path, size_t &sz) {
  int fd = open(path, O_RDONLY);
  sz = lseek(fd, 0, SEEK_END);
  auto *p = static_cast<char *>(
      mmap(nullptr, sz + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (sz)
    mmap(p, sz, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
  return close(fd), p;
}
inline u32 number(const char *&p) {
  u32 v = *p++ - '0';
  while (u32(*p - '0') < 10)
    v = v * 10 + (*p++ - '0');
  return ++p, v;
}
struct RadixHeap {
  std::vector<Item> b[65];
  size_t sz = 0;
  u64 last = 0;
  int slot(u64 k) const {
    return k == last ? 0 : 64 - __builtin_clzll(k ^ last);
  }
  void clear() {
    for (auto &x : b)
      x.clear();
    sz = last = 0;
  }
  void push(u64 k, u32 u) {
    b[slot(k)].push_back({k, u});
    ++sz;
  }
  // The smallest key (the heap must not be empty).
  u64 top() {
    pop_ready();
    return last;
  }
  Item pop() {
    pop_ready();
    --sz;
    auto e = b[0].back();
    return b[0].pop_back(), e;
  }
  void pop_ready() {
    if (b[0].empty()) {
      int i = 1;
      while (b[i].empty())
        ++i;
      u64 m = INF;
      for (auto &e : b[i])
        if (e.d < m)
          m = e.d;
      last = m;
      for (auto &e : b[i])
        b[slot(e.d)].push_back(e);
      b[i].clear();
    }
  }
};
using Spill = std::vector<std::pair<u32, u32>>;
// The weight of edge e from its list of heavy weights (sorted by edge).
u32 heavy_weight(const Spill &s, u32 e) {
  return std::lower_bound(s.begin(), s.end(), std::pair(e, 0u))->second;
}
struct Distances {
  static constexpr u32 P = 512;
  std::vector<u16> tab;
  std::vector<u32> used;
  std::vector<u64> pool;
  explicit Distances(u32 n) : tab((n + P - 1) / P, 0) { pool.reserve(P * 64); }
  u64 &operator[](u32 v) {
    u16 &pg = tab[v / P];
    if (!pg) {
      used.push_back(v / P);
      pg = used.size();
      if (pool.size() < u64(pg) * P)
        pool.resize(u64(pg) * P, INF);
      else
        std::fill_n(&pool[u64(pg - 1) * P], P, INF);
    }
    return pool[u64(pg - 1) * P + v % P];
  }
  // Read without handing out a page: INF for a node this search never reached.
  u64 get(u32 v) const {
    u16 pg = tab[v / P];
    return pg ? pool[u64(pg - 1) * P + v % P] : INF;
  }
  void reset() {
    for (u32 i : used)
      tab[i] = 0;
    used.clear();
  }
};
template <class T> struct Buf {
  struct Free {
    void operator()(T *q) const { free(q); }
  };
  std::unique_ptr<T[], Free> p;
  explicit Buf(size_t n = 0) : p(static_cast<T *>(calloc(n + 1, sizeof(T)))) {}
  T &operator[](size_t i) const { return p[i]; }
};
template <class F> void run_threads(int n, F &&fn) {
  std::vector<std::thread> pool;
  for (int t = 0; t < n; ++t)
    pool.emplace_back(fn, t);
  for (auto &th : pool)
    th.join();
}
int main() {
  // Plain pages: with transparent huge pages a part-touched array can be
  // backed by whole 2 MB pages and count against peak memory.
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 1 << 16);
  const int nt = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
  size_t gsz, qsz;
  const char *graph = map_file("/data/graph.txt", gsz),
             *qtext = map_file("/data/queries.txt", qsz);
  std::vector<std::pair<u32, u32>> queries;
  for (const char *p = qtext; p < qtext + qsz;) {
    u32 s = number(p);
    queries.emplace_back(s, number(p));
  }
  munmap((void *)qtext, qsz + 4096);
  const char *p = graph;
  const u32 n = number(p);
  number(p);
  std::vector<size_t> cut(nt + 1, gsz);
  cut[0] = p - graph;
  for (int t = 1; t < nt; ++t) {
    size_t c = std::max(cut[t - 1], cut[0] + (gsz - cut[0]) * t / nt);
    while (c < gsz && graph[c - 1] != '\n')
      ++c;
    cut[t] = c;
  }
  // Edges come in batches of 32, so each pass can prefetch every random line
  // a batch touches before touching any. On x86 an atomic add waits for the
  // stores before it to finish; one edge at a time, each edge's cache miss
  // would wait for the previous one's.
  constexpr int BATCH = 32;
  auto parse = [&](int t, auto &&fn) {
    u32 bu[BATCH], bv[BATCH], bw[BATCH];
    int k = 0;
    for (const char *q = graph + cut[t]; q < graph + cut[t + 1];) {
      u32 u = number(q), v = number(q), w = number(q);
      if (u != v)
        bu[k] = u, bv[k] = v, bw[k] = w, ++k;
      if (k == BATCH || (k && q >= graph + cut[t + 1]))
        fn(bu, bv, bw, k), k = 0;
    }
  };
  std::vector<std::atomic<u8>> target(n);
  for (auto [s, t] : queries)
    target[t].store(1, std::memory_order_relaxed);
  Buf<u32> next(n + 1);
  run_threads(nt, [&](int t) {
    parse(t, [&](u32 *bu, u32 *bv, u32 *, int k) {
      for (int j = 0; j < k; ++j)
        __builtin_prefetch(&next[bu[j]], 1), __builtin_prefetch(&target[bv[j]]);
      for (int j = 0; j < k; ++j) {
        std::atomic_ref(next[bu[j]]).fetch_add(1, std::memory_order_relaxed);
        if (target[bv[j]].load(std::memory_order_relaxed))
          target[bv[j]].store(2, std::memory_order_relaxed);
      }
    });
  });
  Buf<u32> start(n + 1);
  for (u32 u = 0, tot = 0; u <= n; ++u)
    start[u] = tot, tot += next[u], next[u] = start[u];
  const u32 m = start[n];
  // One 32-bit word per edge: the head in the low hb bits, the weight above it.
  // A weight that does not fit (about 0.1% of edges, and the injected ones
  // near 2^32) is stored as all ones, with its value in a list sorted by edge.
  const int hb = 32 - __builtin_clz(n | 1);
  const u32 hmask = (1u << hb) - 1, big = ~0u >> hb;
  std::vector<Spill> spill(nt);
  auto sorted = [&] {
    Spill all;
    for (auto &s : spill)
      all.insert(all.end(), s.begin(), s.end()), Spill().swap(s);
    std::sort(all.begin(), all.end());
    return all;
  };
  Buf<u32> link(m);
  run_threads(nt, [&](int t) {
    parse(t, [&](u32 *bu, u32 *bv, u32 *bw, int k) {
      u32 slot[BATCH];
      for (int j = 0; j < k; ++j)
        __builtin_prefetch(&next[bu[j]], 1);
      for (int j = 0; j < k; ++j) {
        slot[j] = std::atomic_ref(next[bu[j]]).fetch_add(1, std::memory_order_relaxed);
        __builtin_prefetch(&link[slot[j]], 1);
      }
      for (int j = 0; j < k; ++j) {
        u32 w = bw[j];
        if (w >= big)
          spill[t].emplace_back(slot[j], w), w = big;
        link[slot[j]] = bv[j] | w << hb;
      }
    });
  });
  Spill heavy = sorted();
  auto weight = [&](u32 e, u32 w) { return w != big ? w : heavy_weight(heavy, e); };
  munmap((void *)graph, gsz + 4096);
  next = Buf<u32>();
  std::vector<std::atomic<u8>> claimed(n);
  auto claim = [&](u32 v) {
    u8 no = 0;
    return !claimed[v].load(std::memory_order_relaxed) &&
           claimed[v].compare_exchange_strong(no, 1);
  };
  std::vector<std::vector<u32>> chain(nt);
  run_threads(nt, [&](int t) {
    auto &ord = chain[t];
    std::vector<u32> seeds;
    u32 scan = u64(n) * t / nt;
    for (u32 seed = n;; seed = n) {
      while (!seeds.empty() && seed == n) {
        if (claim(seeds.back()))
          seed = seeds.back();
        seeds.pop_back();
      }
      while (seed == n && scan < n) {
        if (start[scan + 1] > start[scan] && claim(scan))
          seed = scan;
        ++scan;
      }
      if (seed == n)
        break;
      size_t i = ord.size(), blob = i;
      ord.push_back(seed);
      // Queue order is known ahead: fetch the offsets 8 nodes ahead and the
      // edge lists 4 ahead, so neither waits when its node comes up.
      auto ahead = [&] {
        if (i + 8 < ord.size())
          __builtin_prefetch(&start[ord[i + 8]]);
        if (i + 4 < ord.size())
          __builtin_prefetch(&link[start[ord[i + 4]]]);
      };
      for (; i < ord.size() && ord.size() - blob < 512; ++i) {
        ahead();
        for (u32 e = start[ord[i]];
             e < start[ord[i] + 1] && ord.size() - blob < 512; ++e)
          if (claim(link[e] & hmask))
            ord.push_back(link[e] & hmask);
      }
      for (; i < ord.size(); ++i) {
        ahead();
        for (u32 e = start[ord[i]]; e < start[ord[i] + 1]; ++e)
          if (!claimed[link[e] & hmask].load(std::memory_order_relaxed))
            seeds.push_back(link[e] & hmask);
      }
    }
  });
  Buf<u32> id(n), first(n + 1);
  std::vector<u32> base(nt + 1);
  for (int t = 0; t < nt; ++t)
    base[t + 1] = base[t] + chain[t].size();
  run_threads(nt, [&](int t) {
    u32 k = base[t];
    for (u32 u : chain[t])
      id[u] = k++;
    std::vector<u32>().swap(chain[t]);
  });
  u32 k = base[nt];
  for (u32 u = 0; u < n; ++u)
    if (!claimed[u].load(std::memory_order_relaxed))
      id[u] = k++;
  std::vector<std::atomic<u8>>().swap(claimed);
  run_threads(nt, [&](int t) {
    for (u32 u = u64(n) * t / nt; u < u64(n) * (t + 1) / nt; ++u)
      first[id[u] + 1] = start[u + 1] - start[u];
  });
  for (u32 v = 0; v < n; ++v)
    first[v + 1] += first[v];
  // The same words in the new numbering: heads renumbered, weight bits kept.
  Buf<u32> edges(m);
  run_threads(nt, [&](int t) {
    const u32 lo = u64(n) * t / nt, hi = u64(n) * (t + 1) / nt;
    for (u32 u = lo; u < hi; ++u) {
      // Old ids are visited in order, so the random reads of later nodes can
      // start early: their new offsets and heads' new ids 8 nodes ahead, and
      // the slots they write 4 ahead.
      if (u + 8 < hi) {
        __builtin_prefetch(&first[id[u + 8]]);
        for (u32 e = start[u + 8]; e < start[u + 9]; ++e)
          __builtin_prefetch(&id[link[e] & hmask]);
      }
      if (u + 4 < hi)
        __builtin_prefetch(&edges[first[id[u + 4]]], 1);
      for (u32 e = start[u], j = first[id[u]]; e < start[u + 1]; ++e, ++j) {
        u32 x = link[e];
        if (x >> hb == big)
          spill[t].emplace_back(j, weight(e, big));
        edges[j] = id[x & hmask] | (x & ~hmask);
      }
    }
  });
  link = Buf<u32>(), start = Buf<u32>();
  heavy = sorted();
  // Queries in the new numbering, sorted by source so neighbouring searches
  // share cached pages; then the old-to-new map can go.
  const size_t nq = queries.size();
  std::vector<std::pair<u32, u32>> ends(nq);
  std::vector<u32> order(nq);
  for (u32 i = 0; i < nq; ++i)
    ends[i] = {id[queries[i].first], id[queries[i].second]}, order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return ends[a] < ends[b]; });
  id = Buf<u32>();
  // In-edges in the same word format, for the backward half of each search.
  // Counts go to rfirst[v + 1]; after the prefix sum rfirst[v] is v's first
  // slot and is used as v's cursor, which leaves it at v's end, so shifting
  // the array up by one restores the offsets without a second array.
  Buf<u32> rfirst(n + 1), redges(m);
  auto each_edge = [&](int t, auto &&fn) {
    for (u32 u = u64(n) * t / nt; u < u64(n) * (t + 1) / nt; ++u)
      for (u32 e = first[u]; e < first[u + 1]; ++e)
        fn(u, e, edges[e]);
  };
  run_threads(nt, [&](int t) {
    each_edge(t, [&](u32, u32, u32 x) {
      std::atomic_ref(rfirst[(x & hmask) + 1]).fetch_add(1, std::memory_order_relaxed);
    });
  });
  for (u32 v = 0; v < n; ++v)
    rfirst[v + 1] += rfirst[v];
  run_threads(nt, [&](int t) {
    each_edge(t, [&](u32 u, u32 e, u32 x) {
      u32 j = std::atomic_ref(rfirst[x & hmask]).fetch_add(1, std::memory_order_relaxed);
      redges[j] = u | (x & ~hmask);
      if (x >> hb == big)
        spill[t].emplace_back(j, weight(e, big));
    });
  });
  for (u32 v = n; v > 0; --v)
    rfirst[v] = rfirst[v - 1];
  rfirst[0] = 0;
  Spill rheavy = sorted();

  // Bidirectional Dijkstra: grow whichever side has the smaller queue, record
  // the best s-t length seen where the two sides meet, and stop once the two
  // smallest queued keys add up to at least that length (nothing left can
  // beat it). A road network's ball of radius r/2 around each end holds about
  // half as many nodes as one ball of radius r, so each query settles about
  // half as many. A side that runs dry ends the search: it has reached every
  // node it can, so the best meeting seen is the answer.
  std::vector<u64> ans(nq);
  std::atomic<size_t> qidx{0};
  struct Side {
    const u32 *off, *adj;
    const Spill *heavy;
  };
  const Side out_side{first.p.get(), edges.p.get(), &heavy},
      in_side{rfirst.p.get(), redges.p.get(), &rheavy};
  run_threads(nt, [&](int) {
    // Copies the loop can keep in registers (through the captures it would
    // reload them for every edge).
    const u32 mask = hmask, shift = hb, wide = big;
    Distances dist[2] = {Distances(n), Distances(n)};
    RadixHeap queue[2];
    for (size_t q; (q = qidx++) < nq;) {
      size_t i = order[q];
      auto [s0, t0] = queries[i];
      ans[i] = s0 == t0 ? 0 : INF;
      if (s0 == t0 || target[t0].load(std::memory_order_relaxed) != 2)
        continue;
      auto [s, t] = ends[i];
      u64 best = INF;
      dist[0][s] = 0, queue[0].push(0, s);
      dist[1][t] = 0, queue[1].push(0, t);
      while (queue[0].sz && queue[1].sz && queue[0].top() + queue[1].top() < best) {
        int k = queue[1].sz < queue[0].sz;
        const Side &g = k ? in_side : out_side;
        Distances &mine = dist[k], &other = dist[1 - k];
        auto [d, u] = queue[k].pop();
        if (d > mine[u])
          continue;
        for (u32 e = g.off[u], end = g.off[u + 1]; e < end; ++e) {
          u32 x = g.adj[e], v = x & mask, w = x >> shift;
          u64 nd = d + (w != wide ? w : heavy_weight(*g.heavy, e));
          if (nd < best && nd < mine[v]) {
            mine[v] = nd, queue[k].push(nd, v);
            __builtin_prefetch(&g.off[v]);
            if (u64 o = other.get(v); o != INF)
              best = std::min(best, nd + o);
          }
        }
      }
      ans[i] = best;
      for (int k = 0; k < 2; ++k)
        queue[k].clear(), dist[k].reset();
    }
  });
  std::string out;
  out.reserve(ans.size() * 12);
  for (u64 a : ans)
    out += a == INF ? "-1\n" : std::to_string(a) + '\n';
  FILE *f = fopen("/out/distances.txt", "wb");
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
}