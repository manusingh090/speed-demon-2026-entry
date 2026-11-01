// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads, one pass, hours closed while the log is still being
// parsed.
//
// Parse. The log is memory-mapped and cut into line-aligned pieces, eight per
// thread, claimed in order. The timestamp is always 27 bytes and the 72-hour
// span is fixed, so a line's hour comes from its day and hour digits alone.
// Endpoints are interned in one lock-free table shared by all threads, and
// each line files a 4-byte record (latency, endpoint id, 5xx flag) in its
// hour's own region. A thread unmaps each piece once parsed.
//
// Hours. The log is in about clock order, so an hour is closed as soon as
// every piece starting less than two hours after it is parsed. Its records
// are then counting-sorted by endpoint rank into a scratch buffer while still
// in cache, its region handed back, and three selections per group give the
// percentiles. Each group's line has an exact size, since a number's width
// is known without formatting it.
//
// Text. The output is mapped up front at a size it cannot reach and cut to
// size at the end, so an hour's text is written as soon as every earlier hour
// is sized. If any line turns up in a piece started two hours or more after
// its hour, the run is redone with every hour held open to the end.
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
#include <mutex>
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
              PACKED_MAX = (1u << 19) - 1, SPARE = ~0u;

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

// Hour h's records sit together from records + h * per_hour, filled by every
// thread in 4 KB blocks claimed with one atomic add; a thread's last block in
// an hour is padded with SPARE, which no record can equal (a latency field of
// all ones goes to the side list). An hour is handed back with one call once
// its records are copied out.
constexpr u32 BLOCK = 1024;
static u32 *records;
static size_t per_hour;
static std::atomic<u32> filled[HOURS];

struct Parser {
  u32 *cur[HOURS] = {}, *stop[HOURS] = {};
  std::vector<std::pair<u32, u32>> wide[HOURS];
  const int *last = nullptr;
  bool late = false;

  // Parses piece k. Lines arrive in about clock order, so the hour index is
  // worked out only when the line's "MM-DDTHH" bytes differ from the line
  // before's. A line whose hour may already be closed (piece k starts after
  // that hour's last piece) is not stored; it marks the pass late.
  void run(const char *p, const char *end, int k) {
    bool skip = false;
    for (u64 stamp = 0, at; p < end;) {
      int h;
      memcpy(&at, p + 5, 8);
      if (at != stamp) {
        int day = (p[8] - '0') * 10 + p[9] - '0',
            hr = (p[11] - '0') * 10 + p[12] - '0';
        h = (p[6] == '0' ? day - 30 : day + 1) * 24 + hr - 18, stamp = at;
        skip = k > last[h], hour = h;
      }
      h = hour;
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
      if (__builtin_expect(skip, 0)) {
        late = true;
        continue;
      }
      if (__builtin_expect(lat >= PACKED_MAX, 0)) {
        wide[h].push_back({lat, key});
        continue;
      }
      u32 *&c = cur[h];
      if (c == stop[h])
        c = records + h * per_hour + filled[h].fetch_add(BLOCK),
        stop[h] = c + BLOCK;
      *c++ = lat << KEY_BITS | key;
    }
  }
  int hour = 0;
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

  // An hour holds at most every line (one is at least 39 bytes) plus a part
  // block per thread; only the pages written are ever backed.
  per_hour = (size / 39 / BLOCK + nt + 1) * BLOCK;
  records = (u32 *)mmap(nullptr, HOURS * per_hour * 4, 3,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  const int pieces = 8 * nt;
  std::vector<size_t> cut(pieces + 1, size);
  std::vector<int> first(pieces, HOURS);
  cut[0] = 0;
  for (int k = 1; k < pieces; ++k)
    for (cut[k] = std::max(cut[k - 1], size * k / pieces);
         cut[k] < size && log[cut[k] - 1] != '\n';)
      ++cut[k];
  for (int k = 0; k < pieces; ++k)
    if (const char *p = log + cut[k]; cut[k] < size)
      first[k] = ((p[6] == '0' ? (p[8] - '0') * 10 + p[9] - '0' - 30
                               : (p[8] - '0') * 10 + p[9] - '0' + 1) *
                      24 +
                  (p[11] - '0') * 10 + p[12] - '0' - 18);

  // A group's line keeps the endpoint id; ids are named in the order the
  // table filled, and each closing hour sorts its groups by a ranking of
  // every name seen so far (ranked again only when a name was added).
  struct Line {
    u32 id, n, p50, p95, p99, errors;
  };
  constexpr size_t LINES = (ID_MASK + 1) * sizeof(Line);
  char *lines =
      (char *)mmap(nullptr, HOURS * LINES, 3,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  std::vector<std::string_view> name(ID_MASK + 1);
  std::vector<std::unique_ptr<std::vector<u32>>> rankings;
  auto ppm = [](const Line &l) { return u32(u64(l.errors) * 1000000 / l.n); };
  // The output is mapped at a size no output can reach (a group's line is
  // under three times its log line) and cut to size at the end, so each hour
  // is written as soon as every earlier hour's size is known.
  const size_t room = 3 * size + 4096;
  char *text = (char *)mmap(nullptr, room, 3, MAP_SHARED, out, 0);
  if (ftruncate(out, room) || text == MAP_FAILED)
    return perror("output"), 1;

  // One pass parses the pieces in turn and closes each hour as soon as every
  // piece that may hold it is parsed: those starting less than `window` hours
  // after it. The hour's records are then grouped while still in cache and
  // handed back, and its text is written. A pass that finds a line in a piece
  // started `window` hours or more after it reports late, and the run is
  // redone with every hour held open to the end.
  auto pass = [&](int window) {
    std::vector<int> last(HOURS, -1);
    for (int h = 0; h < HOURS; ++h)
      for (int k = 0; k < pieces; ++k)
        if (first[k] < h + window)
          last[h] = k;
    for (auto &f : filled)
      f = 0;
    std::vector<Parser> parsers(nt);
    std::vector<bool> parsed(pieces), sized(HOURS);
    std::vector<u32> count(HOURS);
    std::vector<u64> at(HOURS + 1), bytes(HOURS);
    std::vector<u32 *> scratch;
    std::mutex m;
    int prefix = 0, next_hour = 0, front = 0, next_text = 0, claim_piece = 0;
    // Under m: the next hour whose pieces are all parsed, or -1.
    auto ready = [&] {
      while (prefix < pieces && parsed[prefix])
        ++prefix;
      return next_hour < HOURS && last[next_hour] < prefix ? next_hour++ : -1;
    };
    auto take_text = [&] {
      std::lock_guard lock(m);
      return next_text < front ? next_text++ : -1;
    };
    auto write = [&](int h) {
      char label[32];
      int day = (h + 18) / 24;
      snprintf(label, 32, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
               day < 2 ? 30 + day : day - 1, (h + 18) % 24);
      char *o = text + at[h];
      for (u32 i = 0; i < count[h]; ++i) {
        const Line &l = ((Line *)(lines + h * LINES))[i];
        o = std::copy_n(label, 21, o);
        o = std::copy(name[l.id].begin(), name[l.id].end(), o);
        for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
          *o = ' ', o += 1 + width(x), put(o, x);
        *o++ = '\n';
      }
      madvise(lines + h * LINES, count[h] * sizeof(Line), MADV_DONTNEED);
    };
    auto close_hour = [&](int h, std::vector<u32> &start,
                          std::vector<u32> &cursor, std::vector<u32> &errors) {
      const std::vector<u32> *ranks;
      u32 *latency;
      {
        std::lock_guard lock(m);
        if (rankings.empty() || rankings.back()->size() != endpoints.count) {
          std::vector<std::pair<std::string_view, u32>> by_name;
          for (u32 i = 0; i < Names::SLOTS; ++i)
            if (u64 v = endpoints.slot[i])
              by_name.push_back({endpoints.name(v), u32(v >> 32) & ID_MASK});
          std::sort(by_name.begin(), by_name.end());
          auto &r = *rankings.emplace_back(new std::vector<u32>);
          for (auto [s, id] : by_name)
            r.push_back(id), name[id] = s;
        }
        ranks = rankings.back().get();
        if (scratch.empty())
          scratch.push_back(
              (u32 *)mmap(nullptr, per_hour * 4, 3,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        latency = scratch.back(), scratch.pop_back();
      }
      // The open ends of the threads' last blocks in this hour are padded.
      for (auto &ps : parsers)
        std::fill(ps.cur[h], ps.stop[h], SPARE);
      auto each = [&](auto &&f) {
        for (u32 *r = records + h * per_hour, *e = r + filled[h]; r < e; ++r)
          if (*r != SPARE)
            f(*r & ID_MASK, *r & ERR, *r >> KEY_BITS);
        for (auto &ps : parsers)
          for (auto [lat, key] : ps.wide[h])
            f(key & ID_MASK, key & ERR, lat);
      };
      std::fill(start.begin(), start.end(), 0);
      std::fill(errors.begin(), errors.end(), 0);
      // Counting sort of the hour's latencies, counted and placed by endpoint
      // id with the groups laid out in rank order.
      each([&](u32 id, u32 err, u32) { ++start[id], errors[id] += err != 0; });
      for (u32 e = 0, at = 0; e < ranks->size(); ++e) {
        u32 id = (*ranks)[e];
        cursor[id] = at, at += start[id], start[id] = cursor[id];
      }
      each([&](u32 id, u32, u32 l) { latency[cursor[id]++] = l; });
      madvise(records + h * per_hour, filled[h] * 4, MADV_DONTNEED);
      u64 sz = 0;
      for (u32 id : *ranks) {
        u32 n = cursor[id] - start[id];
        if (!n)
          continue;
        // Nearest rank p is index ceil(p * n / 100) - 1; each higher rank is
        // selected among the values left after the one before.
        u32 *a = latency + start[id], r50 = (50 * n + 99) / 100 - 1,
            r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
        for (u32 from = 0; u32 r : {r50, r95, r99})
          if (r >= from)
            select(a + from, n - from, r - from), from = r + 1;
        Line &l = ((Line *)(lines + h * LINES))[count[h]++];
        l = {id, n, a[r50], a[r95], a[r99], errors[id]};
        sz += 28 + name[id].size();
        for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
          sz += width(x);
      }
      std::lock_guard lock(m);
      scratch.push_back(latency);
      bytes[h] = sz, sized[h] = true;
      for (; front < HOURS && sized[front]; ++front)
        at[front + 1] = at[front] + bytes[front];
    };

    std::vector<std::thread> pool;
    for (int t = 0; t < nt; ++t)
      pool.emplace_back([&, t] {
        std::vector<u32> start(ID_MASK + 1), cursor(ID_MASK + 1),
            errors(ID_MASK + 1);
        parsers[t].last = last.data();
        for (;;) {
          int k = -1, h = -1;
          {
            std::lock_guard lock(m);
            if ((h = ready()) < 0 && claim_piece < pieces)
              k = claim_piece++;
            else if (h < 0 && next_hour == HOURS)
              break;
          }
          if (h >= 0)
            close_hour(h, start, cursor, errors);
          if (k >= 0) {
            parsers[t].run(log + cut[k], log + cut[k + 1], k);
            uintptr_t a = (uintptr_t(log + cut[k]) + 4095) & ~uintptr_t(4095),
                      b = uintptr_t(log + cut[k + 1]) & ~uintptr_t(4095);
            if (b > a)
              madvise((void *)a, b - a, MADV_DONTNEED);
            std::lock_guard lock(m);
            parsed[k] = true;
          }
          for (int g; (g = take_text()) >= 0;)
            write(g);
          if (h < 0 && k < 0)
            std::this_thread::yield();
        }
      });
    for (auto &th : pool)
      th.join();
    for (int g; (g = take_text()) >= 0;)
      write(g);
    bool late = false;
    for (auto &ps : parsers)
      late |= ps.late;
    return late ? u64(-1) : at[HOURS];
  };

  u64 total = pass(2);
  if (total == u64(-1))
    total = pass(HOURS + 1);
  if (ftruncate(out, total))
    return perror("output"), 1;
  close(out);
}
