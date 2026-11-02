// Speed Demon 2026, Track A (Ingest).
//
// Data layout. The log is mapped read-only, followed by one spare zero page.
// One anonymous reservation holds a fixed-size area per hour (untouched pages
// cost nothing): the hour's rows first, in chunks of 256 claimed by whichever
// thread needs one; later the hour's text, written over its rows; and the
// sort scratch further on. Endpoints get ids from one shared hash table that
// fills in the first moments of the run.
//
// Threads. Every thread runs `Run::parse`. Work is handed out by atomic
// counters (1 MiB claims of the log, then hours). The phases overlap: after
// each claim a thread may summarise an hour that is sealed, meaning the claims
// parsed so far at the front of the log hold a line two hours later, so (the
// log being eight time-ordered streams interleaved) no claim still to come
// should hold one of that hour. If that bet fails on some input, `solve` runs
// the whole job again sealing nothing before the end, so the output is exact
// whatever the order of the lines.
//
// 1. Parse. A claim is read 64 KiB at a time: every newline first, then each
//    line from both ends with no data-dependent branch. Each line becomes a
//    4-byte row in its hour's area. A claim's pages are dropped as soon as it
//    is parsed, and it records the lowest and highest hour it holds, which is
//    all `advance` needs to seal hours and to notice a failed bet.
// 2. Summarise. Sealed hours are taken in order and radix-sorted by latency
//    inside their own pages; dealing the sorted rows out by endpoint keeps
//    each endpoint's latencies sorted, so a percentile is one array read. The
//    text is built over the rows. Once every earlier hour's text exists, an
//    hour's offset in the file is known, and the thread that finds it ready
//    writes it there with `pwrite` and gives its pages back, so only a few
//    hours of rows are alive at once, not all 72.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;

constexpr size_t CLAIM = 1 << 20, BLOCK = 1 << 16; // a block stays in L2 between its two passes
constexpr int HOURS = 72; // the span is fixed: 2026-10-30T18 to 2026-11-02T17
constexpr size_t CHUNK = 256; // rows of an hour's area a thread claims at a time
constexpr u32 SLOTS = 1 << 13, ID_BITS = 11; // a slot's low bits hold id + 1
// A row is id + 1 in 11 bits, an error bit, then the latency in 20 bits. The
// 1% of latencies that need more store ESCAPE here and the full value in the
// next row. A row of 0 marks the unused end of a chunk.
constexpr u32 LATENCY_BITS = 20, ESCAPE = (1 << LATENCY_BITS) - 1;
constexpr u32 DIGIT_BITS = 10, DIGITS = 1 << DIGIT_BITS; // two radix passes over 20 bits

// Bit k is set if byte k of the 8 bytes at p is a space.
inline u32 spaces_in(const char *p) {
  __m128i word = _mm_loadl_epi64((const __m128i *)p);
  return _mm_movemask_epi8(_mm_cmpeq_epi8(word, _mm_set1_epi8(' ')));
}

// A 64-bit hash of the whole name with no length-dependent loop: the first and
// last 32 bytes, bytes outside the name masked off, cover a name of up to 64
// bytes, and a longer one mixes two loads over its middle. Its id bits are
// never all zero, so a live slot is never 0. The tail load starts at most 32
// bytes before the name's end, inside the line (a name starts at byte 32 or
// later); the head load runs at most 32 bytes past the log, into the zero page.
inline u64 name_hash(const char *name, size_t len) {
  alignas(32) static constexpr char WINDOW[96] = {
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  auto load = [](const char *p) { return _mm256_loadu_si256((const __m256i *)p); };
  size_t n = std::min<size_t>(len, 32);
  // At WINDOW + 64 - n the first n bytes are kept; at WINDOW + n the last n.
  __m256i head = _mm256_and_si256(load(name), load(WINDOW + 64 - n));
  __m256i tail = _mm256_and_si256(load(name + len - 32), load(WINDOW + n));
  auto mix = [](__m128i s, __m256i v) {
    s = _mm_aesenc_si128(s, _mm256_castsi256_si128(v));
    return _mm_aesenc_si128(s, _mm256_extracti128_si256(v, 1));
  };
  __m128i m = mix(mix(_mm_cvtsi64_si128(len), head), tail);
  if (len > 64)
    m = mix(mix(m, load(name + 32)), load(name + len - 64));
  // Two more AES rounds mix far better than a multiply, at about the same cost.
  m = _mm_aesenc_si128(m, _mm_set1_epi64x(0x9E3779B97F4A7C15));
  m = _mm_aesenc_si128(m, _mm_set1_epi64x(0xBF58476D1CE4E5B9));
  return (u64(_mm_cvtsi128_si64(m)) ^ u64(_mm_extract_epi64(m, 1))) |
         u64(1) << ID_BITS;
}

// Eight digits at once, as bytes with the first digit lowest: divides by
// 10000, then 100, then 10 across the lanes of one word.
inline u64 eight_digits(u32 value) {
  u64 merged = value / 10000 | u64(value % 10000) << 32;
  u64 hundreds = (merged * 10486 >> 20) & 0x0000007F0000007F;
  u64 pairs = hundreds | (merged - 100 * hundreds) << 16;
  u64 tens = (pairs * 103 >> 10) & 0x000F000F000F000F;
  return tens | (pairs - 10 * tens) << 8;
}

// Writes a space and the number with the same instructions for every length,
// so varying widths do not mispredict: the top two of up to ten digits come
// from one eight-digit group and the low eight from another; leading zero
// digits are the low bytes, skipped by a shift (a sentinel bit keeps one digit
// for 0). The 16-byte store runs past the number; what follows overwrites it.
inline char *put_number(char *w, u32 value) {
  *w = ' ';
  u128 digits = eight_digits(value / 100000000) |
                u128(eight_digits(value % 100000000)) << 64;
  u64 lo = digits, hi = u64(digits >> 64) | u64(1) << 63;
  int skip = lo ? __builtin_ctzll(lo) / 8 : 8 + __builtin_ctzll(hi) / 8;
  u128 zeros = u128(0x3030303030303030) * ((u128(1) << 64) + 1);
  u128 text = (digits + zeros) >> (8 * skip);
  memcpy(w + 1, &text, 16);
  return w + 17 - skip;
}

struct Run {
  const char *input;
  size_t len, stride, spare_at; // an hour's area, and where its scratch starts
  u32 *rows;
  bool early;
  int out;
  std::atomic<size_t> next_claim{0};
  // Every endpoint seen so far: hash >> 11 << 11 | id + 1, 0 for an empty
  // slot. Names are known by their hash alone (53 bits compared): with 1,842
  // names the chance of two colliding is about 2 in 10 billion.
  std::atomic<u64> slots[SLOTS] = {};
  std::mutex names_lock;
  std::vector<std::string_view> names; // by id
  std::shared_ptr<std::vector<std::pair<std::string_view, u32>>> order; // by name
  std::atomic<size_t> claimed[HOURS] = {}; // rows of each hour given out
  // Per claim: 0 until parsed, then 1 << 16 | lowest hour << 8 | highest hour.
  std::vector<std::atomic<u32>> parsed;
  // Per claim: the hours that may be sealed once it is the first claim not
  // parsed, from its first line's time (all hours before the one ending 30
  // minutes before it); none when sealing early is off.
  std::vector<int> closes;
  std::atomic<int> sealed{0}, next_hour{0};
  std::atomic<bool> dirty{false}; // a line turned up in a sealed hour
  std::atomic<u32> progress{0}; // counts parsed claims and finished hours, for idle threads to wait on
  std::atomic<const char *> text[HOURS] = {}; // each hour's text, until written
  size_t text_len[HOURS] = {};
  std::mutex place_lock;
  int placed_hour = 0;
  u64 placed_at = 0;

  // The id of an endpoint name, new if this is its first appearance. Slots
  // are only filled under the lock, so a slot seen empty under it ends the
  // probe: the name is new and goes there. The name is stored before the
  // release store publishes the slot.
  u32 lookup(const char *name, size_t len) {
    u64 hash = name_hash(name, len);
    for (u32 i = hash >> (64 - 13);; i = (i + 1) & (SLOTS - 1)) {
      u64 slot = slots[i].load(std::memory_order_acquire);
      if (slot >> ID_BITS == hash >> ID_BITS)
        return (slot & ((1 << ID_BITS) - 1)) - 1;
      if (slot == 0) {
        std::lock_guard hold(names_lock);
        if (slots[i].load(std::memory_order_acquire) != 0)
          { --i; continue; } // filled meanwhile: look at it again
        u32 id = names.size();
        if (id + 1 >= 1u << ID_BITS)
          _exit(3);
        char *copy = new char[len];
        memcpy(copy, name, len);
        names.push_back({copy, len});
        slots[i].store(hash >> ID_BITS << ID_BITS | (id + 1),
                       std::memory_order_release);
        return id;
      }
    }
  }

  void parse() {
    std::pair<u32 *, u32 *> cursors[HOURS] = {}, cursor = {};
    int hour = 0;
    static thread_local u32 ends[BLOCK / 40 + 1];
    for (;;) {
      advance();
      size_t claim = next_claim.fetch_add(CLAIM, std::memory_order_relaxed);
      if (claim >= len)
        break;
      // A line belongs to the claim holding its newline; the first one starts
      // after the previous claim's last newline, at most one line back.
      const char *line = input + claim;
      while (claim > 0 && line[-1] != '\n')
        --line;
      u64 key = ~u64(0);
      int low = HOURS, high = 0;
      for (size_t start = claim; start < std::min(claim + CLAIM, len); start += BLOCK) {
        // The block's newlines first. Lines are longer than 32 bytes, so a
        // 32-byte step holds at most one, and one unconditional store records
        // it (a step with none writes a junk entry the next store overwrites).
        // The aligned load is valid: blocks start at multiples of 64 KiB in a
        // page-aligned mapping, and the last step reads into the zero page.
        const __m256i newline = _mm256_set1_epi8('\n');
        size_t count = 0;
        for (size_t at = start; at < std::min(start + BLOCK, len); at += 32) {
          __m256i hits = _mm256_cmpeq_epi8(_mm256_load_si256((const __m256i *)(input + at)), newline);
          u32 mask = _mm256_movemask_epi8(hits);
          ends[count] = at + __builtin_ctz(mask | 1u << 31);
          count += mask != 0;
        }
        for (size_t k = 0; k < count; ++k) {
          const char *end = input + ends[k];
          _mm_prefetch(end + BLOCK, _MM_HINT_T0);
          // `clock` is "DDTHH:MM" (bytes 8 to 15 of the timestamp). Oct 30,
          // Oct 31, Nov 1 and Nov 2 are days 0 to 3: the units digit plus one,
          // less one in October, whose tens digit '3' has bit 1 set.
          u64 clock;
          memcpy(&clock, line + 8, 8);
          if (clock << 24 != key) [[unlikely]] {
            // Hours change rarely along the log, so the current hour's cursor
            // stays in registers until the day and hour digits change.
            cursors[hour] = cursor;
            key = clock << 24;
            u64 day = (clock >> 8 & 15) + 1 - (clock >> 1 & 1);
            hour = day * 24 + (clock >> 24 & 15) * 10 + (clock >> 32 & 15) - 18;
            cursor = cursors[hour];
            low = std::min(low, hour), high = std::max(high, hour);
          }
          // The method starts at byte 28, 3 to 7 letters; the name follows.
          const char *name = line + 29 + __builtin_ctz(spaces_in(line + 28));
          // The latency, read back from the newline: its digits are the bytes
          // of the last eight after the last space, kept as values 0 to 9 and
          // joined in three multiply-adds (pairs, fours, eight).
          u32 digits = _lzcnt_u32(spaces_in(end - 8)) - 24, latency = 0;
          if (digits < 8) [[likely]] {
            alignas(16) static constexpr char KEEP_LAST[16] = {0, 0, 0, 0, 0, 0, 0, 0, 15, 15, 15, 15, 15, 15, 15, 15};
            __m128i keep = _mm_loadl_epi64((const __m128i *)(KEEP_LAST + digits));
            __m128i v = _mm_and_si128(_mm_loadl_epi64((const __m128i *)(end - 8)), keep);
            v = _mm_maddubs_epi16(v, _mm_set1_epi16(0x010A));
            v = _mm_madd_epi16(v, _mm_set1_epi32(0x00010064));
            v = _mm_madd_epi16(_mm_packus_epi32(v, v), _mm_set1_epi32(0x00012710));
            latency = _mm_cvtsi128_si32(v);
          } else { // eight digits or more (rare)
            for (digits = 0; end[-1 - int(digits)] != ' '; ++digits)
              ;
            for (const char *d = end - digits; d < end; ++d)
              latency = latency * 10 + (*d - '0');
          }
          // The line ends "<name> <status> <latency>\n", the status 3 digits.
          const char *status = end - digits - 4;
          u32 id = lookup(name, status - 1 - name);
          u32 error = *status == '5';
          // Each thread fills its own chunk of each hour, so a row needs no
          // atomics; only a new chunk touches the shared counter. A row needs
          // two places free, in case it escapes.
          if (cursor.first >= cursor.second) [[unlikely]] {
            size_t at = claimed[hour].fetch_add(CHUNK, std::memory_order_relaxed) + hour * stride;
            cursor = {rows + at, rows + at + CHUNK - 1};
          }
          *cursor.first++ = ((id + 1) << 1 | error) << LATENCY_BITS | std::min(latency, ESCAPE);
          if (latency >= ESCAPE) [[unlikely]]
            *cursor.first++ = latency;
          line = end + 1;
        }
      }
      // Drop the claim's pages now, while every thread is busy, rather than
      // at exit on one thread. A later read of the range (the next claim's
      // first line) refaults the same bytes of the read-only file mapping.
      madvise((void *)(input + claim), std::min(CLAIM, len - claim), MADV_DONTNEED);
      parsed[claim / CLAIM].store(1 << 16 | low << 8 | high, std::memory_order_release);
      progress.fetch_add(1), progress.notify_all();
    }
    // Out of claims: what is left is hours that wait for the claims in flight.
    // An idle thread sleeps until a claim is parsed or an hour finished.
    while (next_hour.load(std::memory_order_relaxed) < HOURS) {
      u32 seen = progress.load();
      if (!advance() && next_hour.load(std::memory_order_relaxed) < HOURS)
        progress.wait(seen);
    }
  }

  // Seals hours: once the parsed claims at the front of the log hold a line
  // of hour h + 2, hour h is sealed (the log's eight streams are each in time
  // order, so an hour's lines are all in the log before the first line two
  // hours later; on seed 1 by at least 0.78 MiB). A claim holding a line of an
  // hour sealed before it sets `dirty`, and `solve` runs again sealing nothing
  // until the whole log is read. Then summarises the next sealed hour, if
  // any, and writes what is ready.
  bool advance() { // whether it summarised an hour
    int seal = HOURS, before = 0; // hours that may have been sealed while
                                  // the claim at hand was still unparsed
    for (size_t c = 0; c < parsed.size(); ++c) {
      before = std::max(before, closes[c]);
      u32 info = parsed[c].load(std::memory_order_acquire);
      if (!info) {
        seal = closes[c];
        break;
      }
      if (int(info >> 8 & 255) < before)
        dirty.store(true, std::memory_order_relaxed);
    }
    for (int s = sealed.load(); s < seal && !sealed.compare_exchange_weak(s, seal);)
      ;
    int hour = next_hour.load(std::memory_order_relaxed);
    while (hour < sealed.load(std::memory_order_acquire))
      if (next_hour.compare_exchange_weak(hour, hour + 1)) {
        std::shared_ptr<std::vector<std::pair<std::string_view, u32>>> by_name;
        {
          // The names may have grown since the last hour was summarised.
          std::lock_guard hold(names_lock);
          if (!order || order->size() != names.size()) {
            auto sorted = std::make_shared<std::vector<std::pair<std::string_view, u32>>>();
            for (u32 id = 0; id < names.size(); ++id)
              sorted->push_back({names[id], id});
            std::sort(sorted->begin(), sorted->end());
            order = sorted;
          }
          by_name = order;
        }
        summarise(hour, *by_name);
        write_ready();
        progress.fetch_add(1), progress.notify_all();
        return true;
      }
    return false;
  }

  // Sorts and summarises one hour, all inside its own area: the rows first,
  // the text over them, and the sort scratch further on.
  void summarise(int hour, const std::vector<std::pair<std::string_view, u32>> &order) {
    u32 *area = rows + hour * stride;
    size_t filled = claimed[hour].load(std::memory_order_relaxed);
    // The scratch starts past any rows the hour can hold, so a line filed
    // late (when the early sealing fails, and this pass is then thrown away)
    // can never land in it, and at least 1 MiB in, as the text written over
    // the rows can be longer than them (never 1 MiB).
    u32 *spare = area + spare_at;
    std::vector<u32> starts(1 << ID_BITS), errors(1 << ID_BITS);
    std::vector<std::pair<u32, u32>> big; // (latency, row) of escaped rows
    // Rows move down over the gaps between chunks; the escaped ones go aside
    // (all longer than the rest, so sorted on their own they follow them).
    static thread_local u32 sizes[2][DIGITS];
    memset(sizes, 0, sizeof sizes);
    size_t count = 0;
    for (size_t chunk = 0; chunk < filled; chunk += CHUNK)
      for (size_t at = chunk; at < chunk + CHUNK && area[at];) {
        u32 row = area[at++];
        u32 group = row >> (LATENCY_BITS + 1);
        ++starts[group], errors[group] += row >> LATENCY_BITS & 1;
        if ((row & ESCAPE) == ESCAPE) {
          big.push_back({area[at++], row});
        } else {
          area[count++] = row;
          ++sizes[0][row & (DIGITS - 1)], ++sizes[1][row >> DIGIT_BITS & (DIGITS - 1)];
        }
      }
    // Two stable counting passes, one per digit, sort the rows by latency.
    for (int pass = 0; pass < 2; ++pass) {
      u32 *from = pass ? spare : area, *to = pass ? area : spare, *hist = sizes[pass];
      for (u32 b = 0, total = 0; b < DIGITS; ++b)
        total += std::exchange(hist[b], total);
      for (size_t i = 0; i < count; ++i)
        to[hist[from[i] >> (pass * DIGIT_BITS) & (DIGITS - 1)]++] = from[i];
    }
    std::sort(big.begin(), big.end());
    // Deals the sorted latencies out by endpoint into the scratch. Group id + 1
    // advances from its start to its end, which is where the next group
    // starts, so starts[id]..starts[id + 1] ends up as the span of id.
    for (u32 g = 0, total = 0; g < starts.size(); ++g)
      total += std::exchange(starts[g], total);
    auto deal = [&](u32 row, u32 latency) { spare[starts[row >> (LATENCY_BITS + 1)]++] = latency; };
    for (size_t i = 0; i < count; ++i)
      deal(area[i], area[i] & ESCAPE);
    for (auto [latency, row] : big)
      deal(row, latency);

    // The hour as text with a trailing space (21 bytes), before every line.
    char label[32];
    int day = (hour + 18) / 24;
    snprintf(label, 32, "2026-%02d-%02dT%02d:00:00Z ", day < 2 ? 10 : 11,
             day < 2 ? 30 + day : day - 1, (hour + 18) % 24);
    char *text_start = (char *)area, *w = text_start;
    for (auto [name, id] : order) {
      // (A span is empty, or inverted only in a pass to be thrown away.)
      if (starts[id + 1] <= starts[id])
        continue;
      const u32 *sorted = spare + starts[id];
      u32 n = starts[id + 1] - starts[id], bad = errors[id + 1];
      auto at_rank = [&](u32 p) { return sorted[(p * n + 99) / 100 - 1]; };
      memcpy(w, label, 21);
      memcpy(w + 21, name.data(), name.size());
      w += 21 + name.size();
      for (u32 x : {n, at_rank(50), at_rank(95), at_rank(99), bad})
        w = put_number(w, x);
      w = put_number(w, u64(bad) * 1000000 / n);
      *w++ = '\n';
    }
    // The ranks are read: the scratch pages go back now.
    madvise(spare, (count + big.size()) * 4, MADV_DONTNEED);
    text_len[hour] = w - text_start;
    text[hour].store(text_start, std::memory_order_release);
  }

  // Gives every ready hour whose predecessors are placed its offset in the
  // file, then writes those hours outside the lock, so writes run in several
  // threads at once, and gives each hour's pages back once it is out.
  void write_ready() {
    std::vector<std::pair<int, u64>> ready;
    {
      std::lock_guard hold(place_lock);
      while (placed_hour < HOURS && text[placed_hour].load(std::memory_order_acquire)) {
        ready.push_back({placed_hour, placed_at});
        placed_at += text_len[placed_hour++];
      }
    }
    for (auto [hour, offset] : ready) {
      if (pwrite(out, text[hour].load(), text_len[hour], offset) != ssize_t(text_len[hour]))
        perror("pwrite"), _exit(1);
      madvise(rows + hour * stride, stride * 4, MADV_DONTNEED);
    }
  }
};

// Returns whether the early sealing failed, so the job must run again.
bool solve(bool early) {
  int in = open("/data/requests.log", O_RDONLY);
  int out = open("/out/summary.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (in < 0 || out < 0)
    perror("open"), _exit(1);
  size_t len = lseek(in, 0, SEEK_END);
  int threads = std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
  // Huge pages would make each hour's first touch 2 MB of memory.
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  char *input = (char *)mmap(nullptr, (len + 4095) / 4096 * 4096 + 4096, PROT_READ,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (len && mmap(input, len, PROT_READ, MAP_PRIVATE | MAP_FIXED, in, 0) == MAP_FAILED)
    perror("mmap"), _exit(1);
  // An hour has room for every line's rows (two per line, if all escaped,
  // and one partly filled chunk per thread), then for scratch twice that, so
  // even a pass with lines filed late stays inside its area. Only the pages
  // touched cost anything; areas are whole 2 MiB, so each hour's pages share
  // page tables with no other hour.
  size_t cap = 2 * (len / 40 + 1) + threads * CHUNK;
  size_t spare_at = (std::max<size_t>(cap, 1 << 18) + 1023) & ~size_t(1023);
  size_t stride = (spare_at + 2 * cap + (1 << 19) - 1) & ~size_t((1 << 19) - 1);
  char *base = (char *)mmap(nullptr, stride * HOURS * 4 + (2 << 20), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  auto run = std::make_unique<Run>();
  run->input = input, run->len = len, run->stride = stride, run->spare_at = spare_at, run->early = early, run->out = out;
  run->rows = (u32 *)(((uintptr_t)base + (2 << 20) - 1) & ~uintptr_t((2 << 20) - 1));
  run->parsed = std::vector<std::atomic<u32>>((len + CLAIM - 1) / CLAIM);
  for (size_t c = 0; c < run->parsed.size(); ++c) {
    const char *line = input + c * CLAIM;
    while (c > 0 && line[-1] != '\n')
      --line;
    // The line's time in minutes from the span's start (as in `parse`).
    int day = (line[9] - '0') + 1 - (line[8] == '3');
    int hour = day * 24 + (line[11] - '0') * 10 + line[12] - '0' - 18;
    int minute = hour * 60 + (line[14] - '0') * 10 + line[15] - '0';
    // Hour h may be sealed once the first unparsed line is at least 30
    // minutes into hour h + 1 (the log's latest lines trail by about ten).
    run->closes.push_back(early && len ? std::clamp((minute - 30) / 60, 0, HOURS) : 0);
  }
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t)
    pool.emplace_back([&] { run->parse(); });
  run->parse();
  for (auto &t : pool)
    t.join();
  close(out);
  return run->dirty;
}

int main() { solve(true) && solve(false); }
