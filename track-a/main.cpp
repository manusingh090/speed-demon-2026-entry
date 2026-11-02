// Speed Demon 2026, Track A (Ingest).
//
// One pool of threads, one pass over the log, each hour finished while the
// rest of the log is still being parsed.
//
// Parse. The log is memory-mapped and cut into line-aligned pieces, eight per
// thread, claimed in order, smaller towards the end so threads finish together. The line ends of each 64-byte block are found at
// once, so no line waits on the parse of the one before; a line is then read
// from both ends. The timestamp is always 27 bytes and the 72-hour span is
// fixed, so a line's hour comes from its day and hour digits alone.
// Endpoints are interned in one lock-free table shared by all threads, and
// each line files a 4-byte record (latency, endpoint id, 5xx flag) in its
// hour's own region. A thread unmaps each piece once parsed.
//
// Hours. The log is in about clock order, so an hour is closed as soon as
// the first unparsed piece starts 30 minutes into the next hour. Its records
// are then radix-sorted by endpoint rank and latency, so each group comes out
// sorted and its percentiles are read off by index, and the region is handed
// back.
// Only the hours still open hold records. A line for an hour already closed
// is caught (each hour counts the threads filing into it), and the pass is
// then redone with every hour held open to the end.
//
// Text. Each group keeps its numbers and the exact size of its line, so an
// hour's place in the output is known once every earlier hour is done. The
// output is mapped up front, and each thread formats hours straight into it:
// once no piece is left to claim, a thread with no hour to finish writes the
// next placed hour, and the rest follow when every hour is done.
#include <algorithm>
#include <atomic>
#include <barrier>
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
// A record packs the latency above a 12-bit key: the 5xx flag and the
// endpoint id plus one, so a place never written (zero) is no record.
// Latencies of 2^20 - 1 us or more (about 1% of lines) take two places: a
// record whose latency field is all ones (ESCAPE), then the raw value.
constexpr u32 KEY_BITS = 12, ERR = 1u << 11, ID_MASK = ERR - 1,
              PACKED_MAX = (1u << 20) - 1, ESCAPE = PACKED_MAX << KEY_BITS;

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

// The hour of the line at p, counted from the span's first: its timestamp is
// always 27 bytes, so the day and hour digits are enough.
inline int hour_of(const char *p) {
  int day = (p[8] - '0') * 10 + p[9] - '0';
  return (p[6] == '0' ? day - 30 : day + 1) * 24 + (p[11] - '0') * 10 + p[12] -
         '0' - 18;
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
  static constexpr u32 SLOTS = 1 << 13, MAX = ID_MASK, STRIDE = 128,
                       BUSY = 0xFFFF;
  std::atomic<uint16_t> slot[SLOTS];
  std::atomic<u32> ids{0};
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
        return id;
      }
      if (length[v - 1] == n && !memcmp(text + (v - 1) * STRIDE, s, n))
        return v - 1;
      ++h;
    }
  }
};
static Names endpoints;

// Endpoint ids in unsigned byte order of their names, and each id's place in
// it (indexed by id plus one, as in a record). A ranking is kept for as long
// as no name has been added since it was made.
struct Ranking {
  u32 ids;
  std::vector<u32> by_rank;
  std::vector<uint16_t> rank_of = std::vector<uint16_t>(ID_MASK + 1);

  Ranking() : ids(endpoints.ids) {
    std::vector<std::pair<std::string_view, u32>> by_name;
    for (u32 i = 0; i < Names::SLOTS; ++i)
      if (u32 v = endpoints.slot[i]; v && v != Names::BUSY)
        by_name.push_back({endpoints.name(v - 1), v - 1});
    std::sort(by_name.begin(), by_name.end());
    for (auto [name, id] : by_name)
      rank_of[id + 1] = by_rank.size(), by_rank.push_back(id);
    if (by_rank.size() != ids) // a name was being added: not kept
      ids = ~0u;
  }
};

// Hour h's records sit together from records + h * per_hour, filled by every
// thread in 4 KB blocks claimed with one atomic add. A thread filing into an
// hour counts itself in active[h] while it does; an hour is closed by setting
// closed[h] first and then finding active[h] zero, so a thread that enters
// a closed hour, or an hour closed while a thread was in it, is always seen
// (by the thread or by the closer) and the pass is redone.
constexpr u32 BLOCK = 1024;
static u32 *records;
static size_t per_hour;
static std::atomic<u32> filled[HOURS];
static std::atomic<int> active[HOURS];
static std::atomic<bool> closed[HOURS], late;

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
          cur[h] = c, stop[h] = lim, --active[h];
        h = hour_of(p), stamp = at;
        ++active[h];
        if (closed[h]) // too late for this pass
          return late.store(true);
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
      u32 key = (endpoints.find(name, n, load(name), load(name + 32)) + 1) |
                (ne[1] == '5' ? ERR : 0);
      p = e + 1;
      if (lat >= PACKED_MAX) [[unlikely]] {
        if (lim - c < 2) // the pair stays in one block
          c = lim;
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
      cur[h] = c, stop[h] = lim, --active[h];
  }

  static u32 *claim(int h) {
    return records + h * per_hour + filled[h].fetch_add(BLOCK);
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

  // An hour holds at most two places for every line (one is at least 40
  // bytes), a part block per thread, and after them its sorted keys, one a
  // line; only the pages written are backed.
  per_hour = (size / 13 / BLOCK + nt + 2) * BLOCK;
  records = (u32 *)mmap(nullptr, HOURS * per_hour * 4, 3,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  // Pieces start at line starts; first[k] is the time of piece k's first line,
  // in minutes from the span's start.
  const int pieces = 8 * nt;
  std::vector<size_t> cut(pieces + 1, size);
  std::vector<int> first(pieces + 1, HOURS * 60);
  cut[0] = 0;
  // Guided sizes: the first half of the pieces are 1.5 times the average, the
  // next quarter 0.75 and the last quarter 0.25, so the parse ends on small
  // pieces and the threads run out of it close together.
  for (int k = 1; k < pieces; ++k) {
    double f = double(k) / pieces;
    f = f <= 0.5 ? 1.5 * f : f <= 0.75 ? 0.375 + 0.75 * f : 0.75 + 0.25 * f;
    for (cut[k] = std::max(cut[k - 1], size_t(size * f));
         cut[k] < size && log[cut[k] - 1] != '\n';)
      ++cut[k];
  }
  for (int k = 0; k < pieces; ++k)
    if (cut[k] < size)
      first[k] = hour_of(log + cut[k]) * 60 + (log[cut[k] + 14] - '0') * 10 +
                 log[cut[k] + 15] - '0';

  // Hour h's count[h] lines sit from lines + h * hour_bytes, each hour on
  // pages of its own, handed back once the hour's text is written.
  struct Line {
    u32 id, n, p50, p95, p99, errors;
  };
  const size_t hour_bytes = (ID_MASK * sizeof(Line) + 4095) & ~size_t(4095);
  char *lines = (char *)mmap(nullptr, HOURS * hour_bytes, 3,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  std::vector<u32> count(HOURS);
  auto ppm = [](const Line &l) { return u32(u64(l.errors) * 1000000 / l.n); };
  // Hour h's text takes bytes[h] and starts at at[h], known once every
  // earlier hour is finished (the first `sized` hours).
  std::vector<u64> at(HOURS + 1), bytes(HOURS);
  std::vector<char> finished(HOURS);
  int sized = 0, next_text = 0;
  // The output is mapped at a size no output reaches (a group's line is less
  // than one and a half times its log line) and cut to size at the end, so
  // an hour's text can be written as soon as its offset is known.
  const size_t room = 3 * size + 4096;
  char *text = (char *)mmap(nullptr, room, 3, MAP_SHARED, out, 0);
  if (ftruncate(out, room) || text == MAP_FAILED)
    return perror("output"), 1;
  auto write = [&](int h) {
    char label[32];
    int day = (h + 18) / 24;
    snprintf(label, 32, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
             day < 2 ? 30 + day : day - 1, (h + 18) % 24);
    char *o = text + at[h];
    // The hour's pages of the output, backed in one call rather than one
    // fault each.
    auto first_page = uintptr_t(o) & ~uintptr_t(4095);
    madvise((void *)first_page, uintptr_t(text + at[h + 1]) - first_page,
            MADV_POPULATE_WRITE);
    for (u32 i = 0; i < count[h]; ++i) {
      const Line &l = ((Line *)(lines + h * hour_bytes))[i];
      std::string_view name = endpoints.name(l.id);
      o = std::copy_n(label, 21, o);
      o = std::copy(name.begin(), name.end(), o);
      for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
        *o = ' ', o += 1 + width(x), put(o, x);
      *o++ = '\n';
    }
    madvise(lines + h * hour_bytes, hour_bytes, MADV_DONTNEED);
  };

  std::mutex lock;
  std::vector<std::unique_ptr<Ranking>> rankings;
  auto ranking = [&]() -> const Ranking & {
    std::lock_guard hold(lock);
    if (rankings.empty() || rankings.back()->ids != endpoints.ids)
      rankings.emplace_back(new Ranking);
    return *rankings.back();
  };

  // Hour h, closed, is sorted by (endpoint rank, latency), the rank above a
  // 20-bit latency in one 31-bit key, by a radix sort of three passes (10, 10
  // and 11 bits) between its records and the unused rest of its region. Every
  // group then comes out sorted at a known place, and its percentiles are
  // read off by index. Latencies too large for 20 bits sort last in their
  // group with the same key; their values are kept, sorted, in a short list.
  auto finish = [&](int h, std::vector<u64> &big) {
    if (active[h]) // a thread is filing into it: too late for this pass
      return late.store(true);
    const Ranking &rank = ranking();
    const u32 ne = rank.by_rank.size();
    u32 *keys = records + h * per_hour, *sorted = keys + filled[h], total = 0;
    u32 low[1024] = {}, mid[1024] = {}, top[ID_MASK + 1] = {},
        errors[ID_MASK + 1] = {};
    big.clear();
    for (u32 *r = keys; r < sorted; ++r) {
      u32 x = *r;
      if (!(x & ID_MASK)) // a place never written, at the end of a block
        continue;
      if (x >> KEY_BITS == PACKED_MAX) [[unlikely]]
        big.push_back(u64(rank.rank_of[x & ID_MASK]) << 32 | *++r);
      u32 e = rank.rank_of[x & ID_MASK], key = e << 20 | x >> KEY_BITS;
      errors[e] += (x & ERR) != 0;
      ++low[key & 1023], ++mid[key >> 10 & 1023], ++top[e];
      keys[total++] = key;
    }
    auto pass = [total](const u32 *from, u32 *to, u32 *at, u32 buckets,
                        int shift, u32 mask) {
      for (u32 b = 0, sum = 0; b < buckets; ++b)
        sum += std::exchange(at[b], sum);
      for (u32 j = 0; j < total; ++j)
        to[at[from[j] >> shift & mask]++] = from[j];
    };
    pass(keys, sorted, low, 1024, 0, 1023);
    pass(sorted, keys, mid, 1024, 10, 1023);
    pass(keys, sorted, top, ne, 20, ID_MASK);
    std::sort(big.begin(), big.end());
    u64 sz = 0;
    for (u32 e = 0, from = 0, b = 0; e < ne; from = top[e++]) {
      u32 n = top[e] - from, large = 0;
      if (!n)
        continue;
      while (b + large < big.size() && big[b + large] >> 32 == e)
        ++large;
      // Nearest rank p is index ceil(p * n / 100) - 1.
      u32 v[3], t = 0;
      for (u32 p : {50, 95, 99}) {
        u32 k = (p * n + 99) / 100 - 1;
        v[t++] = k < n - large ? sorted[from + k] & PACKED_MAX
                               : u32(big[b + k - (n - large)]);
      }
      b += large;
      Line &l = ((Line *)(lines + h * hour_bytes))[count[h]++];
      l = {rank.by_rank[e], n, v[0], v[1], v[2], errors[e]};
      sz += 28 + endpoints.length[l.id];
      for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)})
        sz += width(x);
    }
    madvise(keys, (sorted + total - keys) * 4, MADV_DONTNEED);
    std::lock_guard hold(lock);
    bytes[h] = sz, finished[h] = true;
    for (; sized < HOURS && finished[sized]; ++sized)
      at[sized + 1] = at[sized] + bytes[sized];
  };

  // The pass. Each thread closes and finishes the next hour once the first
  // piece not yet parsed starts 30 minutes into the hour after it (the log's
  // shards send an hour's last lines 5 to 10 minutes after it ends; a later
  // one costs a second pass) or every piece is parsed (in a second pass),
  // and otherwise parses the next piece.
  std::vector<Parser> parsers(nt);
  std::vector<char> parsed(pieces + 1);
  int front = 0, next_piece = 0, next_hour = 0, passes = 1;
  bool stream = true;
  // If the pass was too late, everything is handed back and it is redone.
  std::barrier passed(nt, [&]() noexcept {
    if (!late)
      return;
    madvise(records, HOURS * per_hour * 4, MADV_DONTNEED);
    madvise(lines, HOURS * hour_bytes, MADV_DONTNEED);
    for (int h = 0; h < HOURS; ++h)
      filled[h] = active[h] = count[h] = finished[h] = 0, closed[h] = false;
    sized = next_text = 0;
    for (auto &p : parsers)
      p = Parser();
    std::fill(parsed.begin(), parsed.end(), 0);
    front = next_piece = next_hour = 0, stream = late = false, ++passes;
  });
  std::vector<std::thread> pool;
  for (int t = 0; t < nt; ++t)
    pool.emplace_back([&, t] {
      std::vector<u64> big;
      for (int pass = 0; pass < passes; ++pass) {
        while (!late) {
          int h = -1, k = -1, g = -1;
          {
            std::lock_guard hold(lock);
            while (front < pieces && parsed[front])
              ++front;
            if (next_hour < HOURS &&
                (front == pieces || (stream && first[front] >= (next_hour + 1) * 60 + 30)))
              h = next_hour++, closed[h] = true;
            else if (next_piece < pieces)
              k = next_piece++;
            else if (next_text < sized)
              g = next_text++;
            else if (next_hour == HOURS)
              break;
          }
          if (h >= 0)
            finish(h, big);
          else if (g >= 0)
            write(g);
          else if (k >= 0) {
            parsers[t].run(log + cut[k], log + cut[k + 1]);
            uintptr_t a = (uintptr_t(log + cut[k]) + 4095) & ~uintptr_t(4095),
                      b = uintptr_t(log + cut[k + 1]) & ~uintptr_t(4095);
            if (b > a)
              madvise((void *)a, b - a, MADV_DONTNEED);
            std::lock_guard hold(lock);
            parsed[k] = true;
          } else
            std::this_thread::yield();
        }
        passed.arrive_and_wait();
      }
      // The rest of the text, every hour's offset now known.
      for (;;) {
        int g;
        {
          std::lock_guard hold(lock);
          if ((g = next_text++) >= HOURS)
            break;
        }
        write(g);
      }
    });
  for (auto &th : pool)
    th.join();
  if (ftruncate(out, at[HOURS]))
    return perror("output"), 1;
  close(out);
}
