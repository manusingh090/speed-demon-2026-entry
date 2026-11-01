// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads does everything in three phases.
//
// Parse. The log is memory-mapped and cut into line-aligned pieces, eight per
// thread, claimed in turn. The timestamp is always 27 bytes and the 72-hour
// span is fixed, so a line's hour comes from its day and hour digits alone.
// One 64-byte scan after the timestamp finds the endpoint's ends and the
// line's end. Endpoints are interned in one lock-free table shared by all
// threads, and each line files a 4-byte record (latency, endpoint id, 5xx
// flag) under its hour. A thread unmaps each piece once parsed, so the exit
// does not tear the whole mapping down on one thread.
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
// A record packs the latency above a 13-bit key: a 12-bit endpoint id and the
// 5xx flag. Latencies of 2^19 us or more (about 3% of lines) do not fit and go
// to a side list with their key instead.
constexpr u32 KEY_BITS = 13, ERR = 1u << 12, ID_MASK = ERR - 1,
              PACKED_MAX = 1u << 19;

// Bit i is set when byte i of the 32 at p equals c.
inline u32 eq32(const char *p, char c) {
  __m256i v = _mm256_loadu_si256((const __m256i *)p);
  return _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8(c)));
}

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

// The value of the len (1 to 10) digits ending just before end, with no loop:
// the 16 bytes before end, less '0' and with the bytes ahead of the number
// zeroed, combined 2, 4 and then 8 digits at a time.
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

// The first c at or after s; one is known to follow.
inline const char *next_byte(const char *s, char c) {
  for (;; s += 32)
    if (u32 m = eq32(s, c))
      return s + __builtin_ctz(m);
}

// Endpoint names to dense ids, one table for all threads. Open addressing on
// a CRC of the name's first and last eight bytes. A slot holds 20 bits of the
// hash, the id, the length and the name's offset in the arena, so a probe
// compares names only when hash bits and length agree. A new name is copied
// into the arena and published with one compare-and-swap; a thread that loses
// the race compares against the winner's name.
struct Names {
  static constexpr u32 SLOTS = 1 << 13, ARENA = 1 << 19;
  std::atomic<u64> slot[SLOTS];
  std::atomic<u32> used{0}, next_id{0}, count{0};
  char arena[ARENA + 64];

  std::string_view name(u64 v) const {
    return {arena + (v & 0xFFFFF), size_t((v >> 20) & 0xFFF)};
  }

  u32 operator()(std::string_view s) {
    u64 a, b = 0, n = s.size();
    memcpy(&a, s.data(), 8);
    if (n >= 8)
      memcpy(&b, s.data() + n - 8, 8);
    else
      a &= (u64(1) << 8 * n) - 1;
    u64 h = _mm_crc32_u64(_mm_crc32_u64(n, a), b);
    for (u32 i = h;; ++i) {
      u32 j = i % SLOTS;
      u64 v = slot[j].load(std::memory_order_acquire);
      if (!v) {
        u32 at = used.fetch_add(n), id = next_id.fetch_add(1);
        memcpy(arena + at, s.data(), n);
        u64 nv = (u64(h >> 12) << 44) | (u64(id) << 32) | (n << 20) | at;
        if (slot[j].compare_exchange_strong(v, nv))
          return ++count, id;
      }
      if ((v >> 44) == (h >> 12) && ((v >> 20) & 0xFFF) == n &&
          same(name(v), s))
        return u32(v >> 32) & 0xFFF;
    }
  }

  // 64 bytes can always be read from either name (the rest of the line and
  // the spare page follow one, the arena's spare bytes the other), so a name
  // of up to 64 bytes is compared in two steps.
  static bool same(std::string_view x, std::string_view s) {
    if (s.size() > 64)
      return x == s;
    auto eq = [&](int i) {
      __m256i a = _mm256_loadu_si256((const __m256i *)(x.data() + i));
      __m256i b = _mm256_loadu_si256((const __m256i *)(s.data() + i));
      return u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b)));
    };
    return (~(eq(0) | u64(eq(32)) << 32) & (~u64(0) >> (64 - s.size()))) == 0;
  }
};
static Names endpoints;

// One hour's records from one thread, in a chain of 16 KB chunks carved in
// order from a region the thread maps up front.
struct Chunk {
  static constexpr u32 CAP = (16384 - 16) / 4;
  Chunk *prev;
  u32 n, hour, rec[CAP];
};

struct Parser {
  Chunk *last[HOURS] = {}, *next = nullptr, *kept = nullptr;
  int h = 0;
  std::vector<std::pair<u32, u32>> wide[HOURS];

  void map(size_t size) {
    size_t bytes = (size / 40 / Chunk::CAP + 1 + HOURS) * sizeof(Chunk);
    next = kept =
        (Chunk *)mmap(nullptr, bytes, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }

  // Lines arrive in about clock order, so the hour index is worked out only
  // when the line's "MM-DDTHH" bytes differ from the line before's.
  void run(const char *p, const char *end) {
    for (u64 stamp = 0, at; p < end;) {
      memcpy(&at, p + 5, 8);
      if (at != stamp) {
        int day = (p[8] - '0') * 10 + p[9] - '0',
            hr = (p[11] - '0') * 10 + p[12] - '0';
        h = (p[6] == '0' ? day - 30 : day + 1) * 24 + hr - 18, stamp = at;
      }
      const char *m = p + 28;
      u64 sp = eq32(m, ' ') | u64(eq32(m + 32, ' ')) << 32;
      const char *name = m + __builtin_ctzll(sp) + 1;
      sp &= sp - 1;
      const char *ne = sp ? m + __builtin_ctzll(sp) : next_byte(m + 64, ' ');
      // After " SSS " come the latency's digits (bit 0x10 set) and the LF
      // (clear). Up to 7 digits are decoded in one word: moved to its top,
      // so the bytes below read as leading zeros, then combined 2, 4 and 8
      // at a time with multiplies. Longer latencies take the SIMD path.
      u64 w, digits;
      memcpy(&w, ne + 5, 8);
      u32 len, lat;
      if (u64 lf = ~w & 0x1010101010101010) {
        len = __builtin_ctzll(lf) >> 3;
        digits = (w << (64 - 8 * len)) & 0x0F0F0F0F0F0F0F0F;
        digits = (digits * 2561) >> 8 & 0x00FF00FF00FF00FF;
        digits = (digits * 6553601) >> 16 & 0x0000FFFF0000FFFF;
        lat = (digits * 42949672960001) >> 32;
      } else
        len = __builtin_ctz(eq32(ne + 5, '\n')),
        lat = decode(ne + 5 + len, len);
      u32 key = endpoints({name, size_t(ne - name)}) | (ne[1] == '5' ? ERR : 0);
      p = ne + 6 + len;
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

  // The log is in arrival order, so chunks are carved roughly hour by hour and
  // hours finish in the same order. The run of chunks at the front whose hours
  // are all finished is handed back in 64 KB batches, before the output's
  // pages start to count.
  void release(const std::atomic<bool> *done) {
    Chunk *c = kept;
    while (c < next && done[c->hour])
      ++c;
    if (c - kept >= 4 || (c == next && c > kept))
      madvise(kept, (c - kept) * sizeof(Chunk), MADV_DONTNEED), kept = c;
  }

  // Calls f(endpoint id, 5xx flag, latency) for every record of hour h.
  template <class F> void each(int h, F &&f) const {
    for (const Chunk *c = last[h]; c; c = c->prev)
      for (u32 i = 0; i < c->n; ++i)
        f(c->rec[i] & ID_MASK, c->rec[i] & ERR, c->rec[i] >> KEY_BITS);
    for (auto [lat, key] : wide[h])
      f(key & ID_MASK, key & ERR, lat);
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
  std::vector<u32> by_rank;
  u32 most = 0;
  // Endpoints ranked in unsigned byte order, the order of an hour's lines.
  // The first thread out of pieces ranks the names seen so far while the
  // others finish theirs, and the barrier ranks again only if a name was
  // added since (on the full log, never). Each thread adds its own hours'
  // sizes before it arrives, while its chunks are still in its cache.
  std::atomic_flag ranking;
  std::vector<std::atomic<u32>> count(HOURS);
  auto rank_names = [&] {
    std::vector<std::pair<std::string_view, u32>> by_name;
    for (u32 i = 0; i < Names::SLOTS; ++i)
      if (u64 v = endpoints.slot[i])
        by_name.push_back({endpoints.name(v), u32(v >> 32) & 0xFFF});
    std::sort(by_name.begin(), by_name.end());
    names.clear(), by_rank.clear();
    for (auto [name, id] : by_name)
      by_rank.push_back(id), names.push_back(name);
  };
  std::barrier ranked(nt, [&]() noexcept {
    for (auto &n : count)
      most = std::max(most, n.load());
    if (names.size() != endpoints.count)
      rank_names();
  });

  std::vector<std::atomic<bool>> finished(HOURS);
  std::atomic_flag releasing;
  struct Line {
    u32 e, n, p50, p95, p99, errors;
  };
  auto ppm = [](const Line &l) { return u32(u64(l.errors) * 1000000 / l.n); };
  std::vector<std::vector<Line>> lines(HOURS);
  std::vector<u64> at(HOURS + 1);
  char *text = nullptr;
  // Every hour is sized: offsets, then the output mapped at its final size.
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
      if (!ranking.test_and_set())
        rank_names();
      for (int h = 0; h < HOURS; ++h) {
        count[h] += parsers[t].wide[h].size();
        for (const Chunk *c = parsers[t].last[h]; c; c = c->prev)
          count[h] += c->n;
      }
      ranked.arrive_and_wait();
      {
        const u32 ne = names.size(), ids = endpoints.next_id;
        std::vector<u32> start(ids), errors(ids), cursor(ids);
        std::unique_ptr<u32[]> latency(new u32[most]);
        for (int h; (h = claim++) < HOURS;) {
          std::fill(start.begin(), start.end(), 0);
          std::fill(errors.begin(), errors.end(), 0);
          // Counting sort of the hour's latencies, counted and placed by
          // endpoint id with the groups laid out in rank order.
          for (auto &ps : parsers)
            ps.each(h, [&](u32 id, u32 err, u32) {
              ++start[id], errors[id] += err != 0;
            });
          for (u32 e = 0, at = 0; e < ne; ++e) {
            u32 id = by_rank[e];
            cursor[id] = at, at += start[id], start[id] = cursor[id];
          }
          for (auto &ps : parsers)
            ps.each(h, [&](u32 id, u32, u32 l) { latency[cursor[id]++] = l; });
          finished[h] = true;
          if (!releasing.test_and_set()) {
            for (auto &ps : parsers)
              ps.release(finished.data());
            releasing.clear();
          }
          u64 sz = 0;
          for (u32 e = 0; e < ne; ++e) {
            u32 id = by_rank[e], n = cursor[id] - start[id];
            if (!n)
              continue;
            // Nearest rank p is index ceil(p * n / 100) - 1; each higher rank
            // is selected among the values left after the one before.
            u32 *a = latency.get() + start[id], r50 = (50 * n + 99) / 100 - 1,
                r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
            for (u32 from = 0; u32 r : {r50, r95, r99})
              if (r >= from)
                select(a + from, n - from, r - from), from = r + 1;
            const Line &l =
                lines[h].emplace_back(e, n, a[r50], a[r95], a[r99], errors[id]);
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
            *o = ' ', o += 1 + width(x), put(o, x);
          *o++ = '\n';
        }
        std::vector<Line>().swap(lines[h]);
      }
    });
  for (auto &th : pool)
    th.join();
  close(out);
}
