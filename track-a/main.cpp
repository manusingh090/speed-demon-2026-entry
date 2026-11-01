#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;
constexpr int HOURS = 72;
constexpr u32 KEY_BITS = 13, ERR = 1u << 12, ID_MASK = ERR - 1,
              PACKED_MAX = 1u << 19;

inline u32 eq32(const char *p, char c) {
  __m256i v = _mm256_loadu_si256((const __m256i *)p);
  return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}

inline u32 width(u32 x) {
  static constexpr u32 p10[] = {1,         10,        100,       1000,
                                10000,     100000,    1000000,   10000000,
                                100000000, 1000000000};
  u32 t = (32 - __builtin_clz(x | 1)) * 1233 >> 12;
  return t + ((x | 1) >= p10[t]);
}

inline u32 decode(const char *end, u32 len) {
  __m128i v = _mm_subs_epu8(_mm_loadu_si128((const __m128i *)(end - 16)),
                            _mm_set1_epi8('0'));
  __m128i lane =
      _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  v = _mm_and_si128(v, _mm_cmpgt_epi8(lane, _mm_set1_epi8(char(15 - len))));
  v = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));
  v = _mm_madd_epi16(v, _mm_set1_epi32(0x00010064));
  v = _mm_madd_epi16(_mm_packus_epi32(v, v), _mm_set1_epi32(0x00012710));
  return u32(_mm_cvtsi128_si32(v)) * u64(100000000) +
         u32(_mm_extract_epi32(v, 1));
}

inline void select(u32 *a, u32 n, u32 k) {
  for (int round = 0; n > 1; ++round) {
    if (k == 0)
      return std::iter_swap(a, std::min_element(a, a + n));
    if (k + 1 == n)
      return std::iter_swap(a + k, std::max_element(a, a + n));
    if (round == 40)
      return std::nth_element(a, a + k, a + n);
    u32 m = n / 2, x = a[0], y = a[m], z = a[n - 1];
    if (y < x)
      std::swap(x, y);
    if (z < y)
      std::swap(y, z);
    if (y < x)
      std::swap(x, y);
    a[0] = x, a[m] = z, a[n - 1] = y;
    u32 lt = 0;
    for (u32 i = 0; i + 1 < n; ++i) {
      u32 v = a[i];
      a[i] = a[lt], a[lt] = v, lt += v < y;
    }
    std::swap(a[lt], a[n - 1]);
    if (k == lt)
      return;
    if (k < lt)
      n = lt;
    else
      a += lt + 1, k -= lt + 1, n -= lt + 1;
  }
}

inline const char *next_byte(const char *s, char c) {
  for (;; s += 32)
    if (u32 m = eq32(s, c))
      return s + __builtin_ctz(m);
}

struct Names {
  static constexpr u32 SLOTS = 1 << 12, ARENA = 1 << 19;
  std::atomic<u64> slot[SLOTS];
  std::atomic<u32> used{0};
  char arena[ARENA + 64];

  std::string_view name(u64 v) const {
    return {arena + (v & 0xFFFFF), size_t((v >> 20) & 0xFFF)};
  }

  [[gnu::noinline, gnu::cold]] bool insert(u32 j, u64 &v, u64 tag,
                                           std::string_view s) {
    u32 at = used.fetch_add(s.size());
    memcpy(arena + at, s.data(), s.size());
    return slot[j].compare_exchange_strong(v, tag | at);
  }

  u32 operator()(std::string_view s) {
    u64 a, b = 0, n = s.size();
    memcpy(&a, s.data(), 8);
    if (__builtin_expect(n >= 8, 1))
      memcpy(&b, s.data() + n - 8, 8);
    else
      a &= (u64(1) << 8 * n) - 1;
    u64 h = _mm_crc32_u64(_mm_crc32_u64(n, a), b), tag = h << 32 | (n << 20);
    for (u32 i = h;; ++i) {
      u32 j = i % SLOTS;
      u64 v = slot[j].load(std::memory_order_acquire);
      if (__builtin_expect(!v, 0) && insert(j, v, tag, s))
        return j;
      if ((v & ~0xFFFFFull) == tag && same(arena + (v & 0xFFFFF), s))
        return j;
    }
  }

  static bool same(const char *x, std::string_view s) {
    auto eq = [&](size_t i) {
      __m256i a = _mm256_loadu_si256((const __m256i *)(x + i));
      __m256i b = _mm256_loadu_si256((const __m256i *)(s.data() + i));
      return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)));
    };
    if (__builtin_expect(s.size() <= 64, 1))
      return (~(eq(0) | u64(eq(32)) << 32) & (~u64(0) >> (64 - s.size()))) == 0;
    for (size_t i = 0; i < s.size(); i += 32)
      if (~eq(i) & (s.size() - i >= 32 ? ~0u : (1u << (s.size() - i)) - 1))
        return false;
    return true;
  }
};
static Names endpoints;

struct Chunk {
  static constexpr u32 CAP = (16384 - 16) / 4;
  Chunk *prev;
  u32 n, hour, rec[CAP];
};

struct Parser {
  Chunk *last[HOURS] = {}, *next = nullptr, *kept = nullptr;
  std::vector<std::pair<u32, u32>> wide[HOURS];

  void map(size_t size) {
    size_t bytes = (size / 40 / Chunk::CAP + 1 + HOURS) * sizeof(Chunk);
    next = kept =
        (Chunk *)mmap(nullptr, bytes, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  void run(const char *p, const char *end) {
    while (p < end) {
      int day = (p[8] - '0') * 10 + p[9] - '0',
          hr = (p[11] - '0') * 10 + p[12] - '0',
          h = (day + (p[6] - '0') * 31 - 30) * 24 + hr - 18;
      const char *m = p + 28;
      u64 sp = eq32(m, ' ') | u64(eq32(m + 32, ' ')) << 32;
      u64 nl = eq32(m, '\n') | u64(eq32(m + 32, '\n')) << 32;
      const char *name = m + __builtin_ctzll(sp) + 1;
      const char *ne = nl ? m + __builtin_ctzll(sp & (sp - 1))
                          : next_byte(name, ' ');
      const char *eol = nl ? m + __builtin_ctzll(nl) : next_byte(ne + 5, '\n');
      u32 key = endpoints({name, size_t(ne - name)}) | (ne[1] == '5' ? ERR : 0);
      u32 lat = decode(eol, eol - ne - 5);
      p = eol + 1;
      if (__builtin_expect(lat >= PACKED_MAX, 0)) {
        wide[h].push_back({lat, key});
        continue;
      }
      Chunk *&c = last[h];
      if (!c || c->n == Chunk::CAP)
        next->prev = c, next->n = 0, next->hour = h, c = next++;
      c->rec[c->n++] = lat << KEY_BITS | key;
    }
  }

  void release(const std::atomic<bool> *done) {
    Chunk *c = kept;
    while (c < next && done[c->hour])
      ++c;
    if (c - kept >= 4 || (c == next && c > kept))
      madvise(kept, (c - kept) * sizeof(Chunk), MADV_DONTNEED), kept = c;
  }

  template <class F> void each(int h, const u32 *rank, F &&f) const {
    for (const Chunk *c = last[h]; c; c = c->prev)
      for (u32 i = 0; i < c->n; ++i)
        f(rank[c->rec[i] & ID_MASK], c->rec[i] & ERR, c->rec[i] >> KEY_BITS);
    for (auto [lat, key] : wide[h])
      f(rank[key & ID_MASK], key & ERR, lat);
  }
};

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  const int nt = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
  int in = open("/data/requests.log", O_RDONLY);
  int out = open("/out/summary.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (in < 0 || out < 0)
    return perror("open"), 1;

  size_t size = lseek(in, 0, SEEK_END);
  auto *log =
      (char *)mmap(nullptr, size + 4096, 1, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (size && mmap(log, size, 1, MAP_PRIVATE | MAP_FIXED, in, 0) == MAP_FAILED)
    return perror("mmap"), 1;

  const int pieces = 8 * nt;
  std::vector<size_t> cut(pieces + 1, size);
  cut[0] = 0;
  for (int k = 1; k < pieces; ++k)
    for (cut[k] = std::max(cut[k - 1], size * k / pieces);
         cut[k] < size && log[cut[k] - 1] != '\n';)
      ++cut[k];
  std::atomic<int> claim_piece{0}, claim{0}, claim_text{0};

  std::vector<Parser> parsers(nt);
  std::vector<std::string_view> names;
  std::vector<u32> rank(Names::SLOTS);
  u32 most = 0;
  std::barrier ranked(nt, [&]() noexcept {
    for (int h = 0; h < HOURS; ++h) {
      u32 n = 0;
      for (auto &ps : parsers)
        ps.each(h, rank.data(), [&](u32, u32, u32) { ++n; });
      most = std::max(most, n);
    }
    std::vector<std::pair<std::string_view, u32>> by_name;
    for (u32 i = 0; i < Names::SLOTS; ++i)
      if (u64 v = endpoints.slot[i])
        by_name.push_back({endpoints.name(v), i});
    std::sort(by_name.begin(), by_name.end());
    for (auto [name, id] : by_name)
      rank[id] = names.size(), names.push_back(name);
  });

  std::vector<std::atomic<bool>> finished(HOURS);
  std::atomic_flag releasing;
  struct Line {
    u32 e, n, p50, p95, p99, errors;
  };
  auto ppm = [](const Line &l) {
    return l.errors ? u32(u64(l.errors) * 1000000 / l.n) : 0;
  };
  std::vector<std::vector<Line>> lines(HOURS);
  std::vector<u64> at(HOURS + 1);
  char *text = nullptr;
  std::barrier sized(nt, [&]() noexcept {
    for (auto &ps : parsers)
      ps.release(finished.data());
    u64 total = 0;
    for (int h = 0; h < HOURS; ++h)
      total += std::exchange(at[h], total);
    at[HOURS] = total;
    if (!total)
      return;
    if (ftruncate(out, total) ||
        (text = (char *)mmap(nullptr, total, 3, MAP_SHARED, out, 0)) ==
            MAP_FAILED)
      _exit(1);
  });

  std::vector<std::thread> pool;
  for (int t = 0; t < nt; ++t)
    pool.emplace_back([&, t] {
      parsers[t].map(size);
      for (int k; (k = claim_piece++) < pieces;) {
        parsers[t].run(log + cut[k], log + cut[k + 1]);
        uintptr_t a = (uintptr_t(log + cut[k]) + 4095) & ~uintptr_t(4095),
                  b = uintptr_t(log + cut[k + 1]) & ~uintptr_t(4095);
        if (b > a)
          madvise((void *)a, b - a, MADV_DONTNEED);
      }
      ranked.arrive_and_wait();
      {
        const u32 ne = names.size();
        std::vector<u32> start(ne + 1), errors(ne), cursor(ne);
        std::unique_ptr<u32[]> latency(new u32[most]);
        for (int h; (h = claim++) < HOURS;) {
          std::fill(start.begin(), start.end(), 0);
          std::fill(errors.begin(), errors.end(), 0);
          for (auto &ps : parsers)
            ps.each(h, rank.data(), [&](u32 e, u32 err, u32) {
              ++start[e + 1], errors[e] += err != 0;
            });
          for (u32 e = 0; e < ne; ++e)
            cursor[e] = start[e], start[e + 1] += start[e];
          for (auto &ps : parsers)
            ps.each(h, rank.data(),
                    [&](u32 e, u32, u32 l) { latency[cursor[e]++] = l; });
          finished[h] = true;
          if (!releasing.test_and_set()) {
            for (auto &ps : parsers)
              ps.release(finished.data());
            releasing.clear();
          }
          u64 sz = 0;
          for (u32 e = 0; e < ne; ++e) {
            u32 n = start[e + 1] - start[e];
            if (!n)
              continue;
            u32 *a = latency.get() + start[e], r50 = (50 * n + 99) / 100 - 1,
                r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
            for (u32 from = 0; u32 r : {r50, r95, r99})
              if (r >= from)
                select(a + from, n - from, r - from), from = r + 1;
            const Line &l =
                lines[h].emplace_back(e, n, a[r50], a[r95], a[r99], errors[e]);
            sz += 28 + names[e].size();
            for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
              sz += width(x);
          }
          at[h] = sz;
        }
      }
      sized.arrive_and_wait();
      for (int h; (h = claim_text++) < HOURS;) {
        char label[32];
        int day = (h + 18) / 24;
        snprintf(label, 32, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
                 day < 2 ? 30 + day : day - 1, (h + 18) % 24);
        char *o = text + at[h];
        for (const Line &l : lines[h]) {
          o = std::copy_n(label, 21, o);
          o = std::copy(names[l.e].begin(), names[l.e].end(), o);
          for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
            *o++ = ' ', o = std::to_chars(o, o + 10, x).ptr;
          *o++ = '\n';
        }
        std::vector<Line>().swap(lines[h]);
      }
    });
  for (auto &th : pool)
    th.join();
  close(out);
}
