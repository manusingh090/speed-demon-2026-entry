// Speed Demon 2026, Track D (Traverse) — Optimized for K (Time T, Memory M, and
// Source Size S).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <malloc.h>
#include <string>
#include <sys/mman.h>
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
template <class F> void run_threads(int n, F &&fn) {
  std::vector<std::thread> pool;
  for (int t = 0; t < n; ++t)
    pool.emplace_back(fn, t);
  for (auto &th : pool)
    th.join();
}
int main() {
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
  std::vector<std::atomic<u32>> next(n + 1);
  run_threads(nt, [&](int t) {
    parse(t, [&](u32 u, u32 v, u32) {
      next[u].fetch_add(1, std::memory_order_relaxed);
      if (target[v].load(std::memory_order_relaxed))
        target[v].store(2, std::memory_order_relaxed);
    });
  });
  std::vector<u32> start(n + 1);
  for (u32 u = 0, tot = 0; u <= n; ++u) {
    start[u] = tot;
    tot += next[u].load(std::memory_order_relaxed);
    next[u].store(start[u], std::memory_order_relaxed);
  }
  std::vector<u32> heads(start[n]), weights(start[n]);
  run_threads(nt, [&](int t) {
    parse(t, [&](u32 u, u32 v, u32 w) {
      u32 e = next[u].fetch_add(1, std::memory_order_relaxed);
      heads[e] = v;
      weights[e] = w;
    });
  });
  munmap((void *)graph, gsz + 4096);
  std::vector<std::atomic<u32>>().swap(next);
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
          if (claim(heads[e]))
            ord.push_back(heads[e]);
      for (; i < ord.size(); ++i)
        for (u32 e = start[ord[i]]; e < start[ord[i] + 1]; ++e)
          if (!claimed[heads[e]].load(std::memory_order_relaxed))
            seeds.push_back(heads[e]);
    }
  });
  std::vector<u32> id(n), first(n + 1);
  u32 k = 0;
  for (auto &ord : chain) {
    for (u32 u : ord)
      id[u] = k++;
    std::vector<u32>().swap(ord);
  }
  for (u32 u = 0; u < n; ++u)
    if (!claimed[u].load(std::memory_order_relaxed))
      id[u] = k++;
  std::vector<std::atomic<u8>>().swap(claimed);
  for (u32 u = 0; u < n; ++u)
    first[id[u] + 1] = start[u + 1] - start[u];
  for (u32 v = 0; v < n; ++v)
    first[v + 1] += first[v];
  auto renum = [&](std::vector<u32> &old, auto &&fn) {
    std::vector<u32> fresh(old.size());
    run_threads(nt, [&](int t) {
      for (u32 u = u64(n) * t / nt; u < u64(n) * (t + 1) / nt; ++u)
        for (u32 e = start[u], j = first[id[u]]; e < start[u + 1]; ++e, ++j)
          fresh[j] = fn(old[e]);
    });
    old.swap(fresh);
  };
  renum(weights, [](u32 w) { return w; });
  renum(heads, [&](u32 v) { return id[v]; });
  std::vector<u32>().swap(start);
  std::vector<u32> order(queries.size());
  for (u32 i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return id[queries[a].first] < id[queries[b].first];
  });
  std::vector<u64> ans(queries.size());
  std::atomic<size_t> qidx{0};
  const u32 *fp = first.data(), *hp = heads.data(), *wp = weights.data();
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
          u32 v = hp[e];
          u64 nd = d + wp[e];
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