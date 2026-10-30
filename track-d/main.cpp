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
  Item pop() {
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
    --sz;
    auto e = b[0].back();
    return b[0].pop_back(), e;
  }
};
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
  auto parse = [&](int t, auto &&fn) {
    for (const char *q = graph + cut[t]; q < graph + cut[t + 1];) {
      u32 u = number(q), v = number(q), w = number(q);
      if (u != v)
        fn(u, v, w);
    }
  };
  std::vector<std::atomic<u8>> target(n);
  for (auto [s, t] : queries)
    target[t].store(1, std::memory_order_relaxed);
  Buf<u32> next(n + 1);
  run_threads(nt, [&](int t) {
    parse(t, [&](u32 u, u32 v, u32) {
      std::atomic_ref(next[u]).fetch_add(1, std::memory_order_relaxed);
      if (target[v].load(std::memory_order_relaxed))
        target[v].store(2, std::memory_order_relaxed);
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
  using Spill = std::vector<std::pair<u32, u32>>;
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
    parse(t, [&](u32 u, u32 v, u32 w) {
      u32 e = std::atomic_ref(next[u]).fetch_add(1, std::memory_order_relaxed);
      if (w >= big)
        spill[t].emplace_back(e, w), w = big;
      link[e] = v | w << hb;
    });
  });
  Spill heavy = sorted();
  auto weight = [&](u32 e, u32 w) {
    return w != big ? w : std::lower_bound(heavy.begin(), heavy.end(), std::pair(e, 0u))->second;
  };
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
      for (; i < ord.size() && ord.size() - blob < 512; ++i)
        for (u32 e = start[ord[i]];
             e < start[ord[i] + 1] && ord.size() - blob < 512; ++e)
          if (claim(link[e] & hmask))
            ord.push_back(link[e] & hmask);
      for (; i < ord.size(); ++i)
        for (u32 e = start[ord[i]]; e < start[ord[i] + 1]; ++e)
          if (!claimed[link[e] & hmask].load(std::memory_order_relaxed))
            seeds.push_back(link[e] & hmask);
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
    for (u32 u = u64(n) * t / nt; u < u64(n) * (t + 1) / nt; ++u)
      for (u32 e = start[u], j = first[id[u]]; e < start[u + 1]; ++e, ++j) {
        u32 x = link[e];
        if (x >> hb == big)
          spill[t].emplace_back(j, weight(e, big));
        edges[j] = id[x & hmask] | (x & ~hmask);
      }
  });
  link = Buf<u32>(), start = Buf<u32>();
  heavy = sorted();
  std::vector<u32> order(queries.size());
  for (u32 i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return id[queries[a].first] < id[queries[b].first];
  });
  std::vector<u64> ans(queries.size());
  std::atomic<size_t> qidx{0};
  const u32 *fp = first.p.get(), *ep = edges.p.get();
  run_threads(nt, [&](int) {
    Distances dist(n);
    RadixHeap heap;
    for (size_t q; (q = qidx++) < queries.size();) {
      size_t i = order[q];
      auto [s0, t0] = queries[i];
      ans[i] = s0 == t0 ? 0 : INF;
      if (s0 == t0 || target[t0].load(std::memory_order_relaxed) != 2)
        continue;
      u32 s = id[s0], t = id[t0];
      u64 best = INF;
      dist[s] = 0;
      heap.push(0, s);
      while (heap.sz) {
        auto [d, u] = heap.pop();
        if (d >= best || u == t) {
          best = u == t ? d : best;
          break;
        }
        if (d > dist[u])
          continue;
        for (u32 e = fp[u], end = fp[u + 1]; e < end; ++e) {
          u32 v = ep[e] & hmask;
          u64 nd = d + weight(e, ep[e] >> hb);
          if (nd < best && nd < dist[v]) {
            dist[v] = nd;
            if (v == t)
              best = nd;
            else
              heap.push(nd, v);
          }
        }
      }
      ans[i] = best;
      heap.clear();
      dist.reset();
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