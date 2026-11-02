// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads does everything in three phases.
//
// Parse. The log is memory-mapped and cut into line-aligned pieces, eight per
// thread, claimed in turn. The line ends of each 64-byte block are found at
// once, so no line waits on the parse of the one before; a line is then read
// from both ends. The timestamp is always 27 bytes and the 72-hour span is
// fixed, so a line's hour comes from its day and hour digits alone.
// Endpoints are interned in one lock-free table shared by all threads, and
// each line files a 4-byte record (latency, endpoint id, 5xx flag) under its
// hour. A thread unmaps each piece once parsed, so the exit does not tear the
// whole mapping down on one thread.
//
// Between the phases the last thread to arrive sorts the names, so each
// endpoint id gets its rank in output order.
//
// Hours. Threads claim hours. An hour's latencies are counting-sorted by
// endpoint rank into per-thread scratch, and three selections per group give
// the percentiles. Each group keeps its numbers along with the exact size its
// line will take, since a number's width is known without formatting it.
//
// Text. The last thread into the second barrier turns the sizes into offsets
// and maps the output file at its final size; then every thread formats its
// hours straight into it.
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
constexpr int HOURS = 72; // 2026-10-30T18:00Z to 2026-11-02T17:59:59Z
// A record packs the latency above a 12-bit key: an 11-bit endpoint id and the
// 5xx flag. Latencies of 2^20 - 1 us or more (about 1% of lines) do not fit.
constexpr u32 KEY_BITS = 12, ERR = 1u << 11, ID_MASK = ERR - 1,
              PACKED_MAX = (1u << 20) - 1, ESCAPE = PACKED_MAX << KEY_BITS,
              SPARE = ~0u;

// The number of decimal digits in x, from its bit length.
inline u32 width(u32 x) {
  static constexpr u32 p10[] = {1,         10,        100,     1000,
                                10000,     100000,    1000000, 10000000,
                                100000000, 1000000000};
  u32 t = (32 - __builtin_clz(x | 1)) * 1233 >> 12;
  return t + ((x | 1) >= p10[t]);
}

// Writes x in decimal ending at end, whose width is already known, two digits
// at a time from a table of the 100 pairs.
inline void put(char *end, u32 x) {
  static constexpr char pairs[] =
      "000102030405060708091011121314151617181920212223242526272829303132333435"
      "363738394041424344454647484950515253545556575859606162636465666768697071"
      "72737475767778798081828384858687888990919293949596979899";
  for (; x >= 100; x /= 100)
    memcpy(end -= 2, &pairs[x % 100 * 2], 2);
  if (x >= 10)
    memcpy(end - 2, &pairs[x * 2], 2);
  else
    end[-1] = '0' + x;
}

// Puts the k-th smallest of a[0, n) at a[k], the smaller ones before it,
// without a branch on the data: each round partitions around the median of
// three with an unconditional swap. Many equal values could shrink a round by
// one only, so after 40 rounds the library's selection finishes.
inline void select(u32 *a, u32 n, u32 k) {
  for (int round = 0; n > 1; ++round) {
    if (round == 40)
      return std::nth_element(a, a + k, a + n);
    u32 m = n / 2;
    if (a[m] < a[0])
      std::swap(a[m], a[0]);
    if (a[n - 1] < a[m])
      std::swap(a[n - 1], a[m]);
    if (a[m] < a[0])
      std::swap(a[m], a[0]);
    std::swap(a[m], a[n - 1]);
    u32 pivot = a[n - 1], lt = 0;
    for (u32 i = 0; i + 1 < n; ++i) {
      u32 x = a[i];
      a[i] = a[lt], a[lt] = x, lt += x < pivot;
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

// Writes the values at ranks r[0] <= r[1] <= r[2] of a[0, n) to v. A group
// of 16 or fewer (four in five groups) is padded to 16 and sorted by Batcher's
// odd-even merge network, 63 min/max pairs with no branch on the data; its
// first 19 pairs already sort the first 8 places, so a group of 8 or fewer
// stops there. Otherwise each rank is selected among the values left above
// the one before.
inline void percentiles(u32 *a, u32 n, const u32 *r, u32 *v) {
  if (n <= 16) {
    static constexpr int net[63][2] = {
        {0, 1},   {2, 3},   {0, 2},   {1, 3},   {1, 2},   {4, 5},   {6, 7},
        {4, 6},   {5, 7},   {5, 6},   {0, 4},   {2, 6},   {2, 4},   {1, 5},
        {3, 7},   {3, 5},   {1, 2},   {3, 4},   {5, 6},   {8, 9},   {10, 11},
        {8, 10},  {9, 11},  {9, 10},  {12, 13}, {14, 15}, {12, 14}, {13, 15},
        {13, 14}, {8, 12},  {10, 14}, {10, 12}, {9, 13},  {11, 15}, {11, 13},
        {9, 10},  {11, 12}, {13, 14}, {0, 8},   {4, 12},  {4, 8},   {2, 10},
        {6, 14},  {6, 10},  {2, 4},   {6, 8},   {10, 12}, {1, 9},   {5, 13},
        {5, 9},   {3, 11},  {7, 15},  {7, 11},  {3, 5},   {7, 9},   {11, 13},
        {1, 2},   {3, 4},   {5, 6},   {7, 8},   {9, 10},  {11, 12}, {13, 14}};
    u32 s[16];
    for (u32 i = 0; i < 16; ++i)
      s[i] = i < n ? a[i] : ~0u;
    for (int c = 0, pairs = n <= 8 ? 19 : 63; c < pairs; ++c) {
      auto [i, j] = net[c];
      u32 x = std::min(s[i], s[j]);
      s[j] = std::max(s[i], s[j]), s[i] = x;
    }
    for (int t = 0; t < 3; ++t)
      v[t] = s[r[t]];
    return;
  }
  for (u32 from = 0, t = 0; t < 3; ++t)
    if (r[t] >= from)
      select(a + from, n - from, r[t] - from), from = r[t] + 1;
  for (int t = 0; t < 3; ++t)
    v[t] = a[r[t]];
}

inline __m256i load(const char *p) {
  return _mm256_loadu_si256((const __m256i *)p);
}

// Bit i is set when byte i of the 64 in a and b equals the same byte of x, y.
inline u64 eq64(__m256i a, __m256i b, __m256i x, __m256i y) {
  return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, x))) |
         u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, y)))) << 32;
}

// Endpoint names to dense ids, one table for all threads. A slot of the
// open-addressing table (16 KB, so it stays in the core's first cache) holds
// a name's id plus one, found by a CRC of the name's first and last eight
// bytes. Names sit 128 bytes apart, each followed by a space, as a name in a
// line is: the name and its space match a stored one exactly when the names
// are equal, so a name of up to 63 bytes is confirmed by the same two 32-byte
// loads that found its end. A new name is claimed with one compare-and-swap
// (the slot holds BUSY until the name is written), so no thread reads a
// half-written name.
struct Names {
  static constexpr u32 SLOTS = 1 << 13, MAX = 2047, STRIDE = 128,
                       BUSY = 0xFFFF;
  std::atomic<uint16_t> slot[SLOTS];
  std::atomic<u32> ids{0}, count{0};
  uint8_t length[MAX];
  alignas(64) char text[(MAX + 1) * STRIDE];

  std::string_view name(u32 id) const {
    return {text + id * STRIDE, length[id]};
  }

  // The id of the name of length n at s, whose first 64 bytes are v0, v1.
  u32 find(const char *s, u32 n, __m256i v0, __m256i v1) {
    u64 a, b;
    memcpy(&a, s, 8), memcpy(&b, s + n - 8, 8);
    if (n < 8)
      a = _bzhi_u64(a, 8 * n), b = 0;
    for (u32 h = _mm_crc32_u64(_mm_crc32_u64(n, a), b);; ++h) {
      u32 v = slot[h % SLOTS].load(std::memory_order_acquire);
      if (v - 1 >= MAX) [[unlikely]]
        return insert(h, s, n);
      const char *e = text + (v - 1) * STRIDE;
      u64 eq = eq64(v0, v1, load(e), load(e + 32));
      if (n < 64 ? (~eq & _bzhi_u64(~u64(0), n + 1)) == 0
                 : ~eq == 0 && (~eq64(load(s + 64), load(s + 96),
                                      load(e + 64), load(e + 96)) &
                                _bzhi_u64(~u64(0), n - 63)) == 0)
        return v - 1;
    }
  }

  [[gnu::noinline]] u32 insert(u32 h, const char *s, u32 n) {
    for (;;) {
      std::atomic<uint16_t> &at = slot[h % SLOTS];
      uint16_t v = at.load(std::memory_order_acquire);
      if (v == BUSY)
        continue;
      if (v == 0) {
        if (!at.compare_exchange_strong(v, BUSY))
          continue;
        u32 id = ids++;
        if (id >= MAX)
          _exit(3);
        memcpy(text + id * STRIDE, s, n);
        text[id * STRIDE + n] = ' ', length[id] = n;
        at.store(id + 1, std::memory_order_release);
        return ++count, id;
      }
      if (length[v - 1] == n && !memcmp(text + (v - 1) * STRIDE, s, n))
        return v - 1;
      ++h;
    }
  }
};
static Names endpoints;

// Hour h's records sit together from records + h * per_hour, filled by every
// thread in 4 KB blocks claimed with one atomic add; a thread's last block in
// an hour is padded with SPARE. A latency of PACKED_MAX or more takes two
// places: a record whose latency field is all ones (ESCAPE), then the raw
// value. An hour is handed back with one call once its records are copied
// out.
constexpr u32 BLOCK = 1024;
static u32 *records;
static size_t per_hour;
static std::atomic<u32> filled[HOURS];

struct Parser {
  u32 *cur[HOURS] = {}, *stop[HOURS] = {};

  // Lines arrive in about clock order, so the hour index is worked out only
  // when the line's "MM-DDTHH" bytes differ from the line before's, and the
  // hour's place in its records stays in registers until then.
  //
  // The line ends of each 64-byte block are found at once and the lines taken
  // from them bit by bit, so a line's start does not wait on the parse of the
  // line before and the core overlaps several lines. A line is read from both
  // ends: the method and the name from its start, the latency (at most 10
  // digits after the last space) and the status back from its LF.
  void run(const char *p, const char *end) {
    const __m256i space = _mm256_set1_epi8(' '), lf = _mm256_set1_epi8('\n');
    const __m128i space16 = _mm256_castsi256_si128(space);
    u32 *c = nullptr, *lim = nullptr;
    int h = -1;
    u64 stamp = 0, at;
    const char *block = p;
    for (u64 ends = eq64(load(block), load(block + 32), lf, lf); p < end;
         ends &= ends - 1) {
      while (!ends) {
        // The log streams through once; fetching it 1 KB ahead with the
        // non-temporal hint keeps it from pushing the endpoint table and the
        // names out of the core's caches.
        _mm_prefetch(block + 1024, _MM_HINT_NTA);
        block += 64, ends = eq64(load(block), load(block + 32), lf, lf);
      }
      const char *e = block + __builtin_ctzll(ends);
      memcpy(&at, p + 5, 8);
      if (at != stamp) [[unlikely]] {
        if (h >= 0)
          cur[h] = c, stop[h] = lim;
        int day = (p[8] - '0') * 10 + p[9] - '0',
            hr = (p[11] - '0') * 10 + p[12] - '0';
        h = (p[6] == '0' ? day - 30 : day + 1) * 24 + hr - 18, stamp = at;
        c = cur[h], lim = stop[h];
      }
      u32 method = _mm_movemask_epi8(_mm_cmpeq_epi8(
          _mm_loadu_si128((const __m128i *)(p + 28)), space16));
      u32 tail = _mm_movemask_epi8(_mm_cmpeq_epi8(
          _mm_loadu_si128((const __m128i *)(e - 16)), space16));
      const char *name = p + 29 + __builtin_ctz(method);
      u32 len = __builtin_clz(tail) - 16; // digits after the last space
      const char *ne = e - len - 5;       // the space before the status
      u32 n = ne - name;
      // Up to 8 digits are decoded in one word: the 8 bytes before the LF,
      // with those below the digits cleared so they read as leading zeros,
      // combined 2, 4 and 8 at a time with multiplies. Longer ones (rare) go
      // digit by digit.
      u32 lat = 0;
      if (len <= 8) [[likely]] {
        u64 digits;
        memcpy(&digits, e - 8, 8);
        digits = digits >> (64 - 8 * len) << (64 - 8 * len) & 0x0F0F0F0F0F0F0F0F;
        digits = (digits * 2561) >> 8 & 0x00FF00FF00FF00FF;
        digits = (digits * 6553601) >> 16 & 0x0000FFFF0000FFFF;
        lat = (digits * 42949672960001) >> 32;
      } else
        for (const char *d = e - len; d < e; ++d)
          lat = lat * 10 + *d - '0';
      u32 key = endpoints.find(name, n, load(name), load(name + 32)) |
                (ne[1] == '5' ? ERR : 0);
      p = e + 1;
      if (lat >= PACKED_MAX) [[unlikely]] {
        if (lim - c < 2) // the pair stays in one block
          std::fill(c, lim, SPARE), c = lim;
        if (c == lim)
          c = claim(h), lim = c + BLOCK;
        *c++ = ESCAPE | key, *c++ = lat;
        continue;
      }
      if (c == lim) [[unlikely]]
        c = claim(h), lim = c + BLOCK;
      *c++ = lat << KEY_BITS | key;
    }
    if (h >= 0)
      cur[h] = c, stop[h] = lim;
  }

  static u32 *claim(int h) {
    return records + h * per_hour + filled[h].fetch_add(BLOCK);
  }

  void seal() {
    for (int h = 0; h < HOURS; ++h)
      std::fill(cur[h], stop[h], SPARE);
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

  // An hour holds at most two places for every line (one is at least 39
  // bytes) plus a part block per thread; only the pages written are backed.
  per_hour = (size / 19 / BLOCK + nt + 1) * BLOCK;
  records = (u32 *)mmap(nullptr, HOURS * per_hour * 4, 3,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
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
  std::vector<u32> by_rank;
  u32 most = 0;
  struct Line {
    u32 e, n, p50, p95, p99, errors;
  };
  // Hour h's count[h] lines sit from lines + h * hour_bytes, each hour on
  // whole pages of its own, handed back once the hour's text is written
  // (freed vectors would stay resident while the output's pages fill).
  char *lines = nullptr;
  size_t hour_bytes = 0;
  std::vector<u32> count(HOURS);
  // Endpoints ranked in unsigned byte order, the order of an hour's lines.
  // The first thread out of pieces ranks the names seen so far while the
  // others finish theirs, and the barrier ranks again only if a name was
  // added since (on the full log, never).
  std::atomic_flag ranking;
  auto rank_names = [&] {
    std::vector<std::pair<std::string_view, u32>> by_name;
    for (u32 i = 0; i < Names::SLOTS; ++i)
      if (u32 v = endpoints.slot[i]; v && v != Names::BUSY)
        by_name.push_back({endpoints.name(v - 1), v - 1});
    std::sort(by_name.begin(), by_name.end());
    names.clear(), by_rank.clear();
    for (auto [name, id] : by_name)
      by_rank.push_back(id), names.push_back(name);
  };
  // Hours are claimed smallest first. Each is handed back on its own, so the
  // order is free: the first twelve, whose sort scratch is touched while
  // every record is still held, are then the smallest, and the threads still
  // finish close together (the largest hours, which come last in clock order
  // too, are spread over all of them).
  std::vector<int> order(HOURS);
  std::barrier ranked(nt, [&]() noexcept {
    for (int h = 0; h < HOURS; ++h)
      most = std::max(most, filled[h].load()), order[h] = h;
    std::sort(order.begin(), order.end(),
              [](int a, int b) { return filled[a] < filled[b]; });
    if (names.size() != endpoints.count)
      rank_names();
    hour_bytes = (names.size() * sizeof(Line) + 4095) & ~size_t(4095);
    lines = (char *)mmap(nullptr, HOURS * hour_bytes, 3,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  });

  // Calls f(endpoint id, 5xx flag, latency) for every record of hour h.
  auto each = [&](int h, auto &&f) {
    for (u32 *r = records + h * per_hour, *e = r + filled[h]; r < e; ++r)
      if (__builtin_expect((*r | ID_MASK | ERR) == SPARE, 0)) {
        if (*r != SPARE) // an escape: the raw latency follows
          f(*r & ID_MASK, *r & ERR, r[1]), ++r;
      } else
        f(*r & ID_MASK, *r & ERR, *r >> KEY_BITS);
  };
  auto ppm = [](const Line &l) { return u32(u64(l.errors) * 1000000 / l.n); };
  std::vector<u64> at(HOURS + 1);
  char *text = nullptr;
  // Every hour is sized: offsets, then the output mapped at its final size.
  std::barrier sized(nt, [&]() noexcept {
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
      for (int k; (k = claim_piece++) < pieces;) {
        parsers[t].run(log + cut[k], log + cut[k + 1]);
        uintptr_t a = (uintptr_t(log + cut[k]) + 4095) & ~uintptr_t(4095),
                  b = uintptr_t(log + cut[k + 1]) & ~uintptr_t(4095);
        if (b > a)
          madvise((void *)a, b - a, MADV_DONTNEED);
      }
      if (!ranking.test_and_set())
        rank_names();
      parsers[t].seal();
      ranked.arrive_and_wait();
      {
        const u32 ne = names.size(), ids = endpoints.ids;
        std::vector<u32> start(ids), errors(ids), cursor(ids);
        std::unique_ptr<u32[]> latency(new u32[most]);
        for (int i; (i = claim++) < HOURS;) {
          int h = order[i];
          std::fill(start.begin(), start.end(), 0);
          std::fill(errors.begin(), errors.end(), 0);
          // Counting sort of the hour's latencies, counted and placed by
          // endpoint id with the groups laid out in rank order.
          each(h, [&](u32 id, u32 err, u32) {
            ++start[id], errors[id] += err != 0;
          });
          for (u32 e = 0, at = 0; e < ne; ++e) {
            u32 id = by_rank[e];
            cursor[id] = at, at += start[id], start[id] = cursor[id];
          }
          each(h, [&](u32 id, u32, u32 l) { latency[cursor[id]++] = l; });
          madvise(records + h * per_hour, filled[h] * 4, MADV_DONTNEED);
          u64 sz = 0;
          for (u32 e = 0; e < ne; ++e) {
            u32 id = by_rank[e], n = cursor[id] - start[id];
            if (!n)
              continue;
            // Nearest rank p is index ceil(p * n / 100) - 1.
            u32 *a = latency.get() + start[id], r50 = (50 * n + 99) / 100 - 1,
                r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
            u32 ranks[3] = {r50, r95, r99}, v[3];
            percentiles(a, n, ranks, v);
            Line &l = ((Line *)(lines + h * hour_bytes))[count[h]++];
            l = {e, n, v[0], v[1], v[2], errors[id]};
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
        for (u32 i = 0; i < count[h]; ++i) {
          const Line &l = ((Line *)(lines + h * hour_bytes))[i];
          o = std::copy_n(label, 21, o);
          o = std::copy(names[l.e].begin(), names[l.e].end(), o);
          for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
            *o = ' ', o += 1 + width(x), put(o, x);
          *o++ = '\n';
        }
        madvise(lines + h * hour_bytes, hour_bytes, MADV_DONTNEED);
      }
    });
  for (auto &th : pool)
    th.join();
  close(out);
}
