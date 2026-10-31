# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

One C++20 file, `main.cpp`, built as a static binary with `-O3 -march=native`. Both input files are memory-mapped (with a spare zero page after each, so wide loads near the end are safe), and every phase runs on one pool of 12 threads separated by `std::barrier`.

1. **Countries.** `regions.csv` is split by lines. Each thread flags the three-letter codes it sees; the barrier's completion step numbers them 1..N in code order, which is output order (`_UNMATCHED` is index 0 and printed last).
2. **Join table.** Each thread fills its slice of a direct table `region_id -> country index` (one byte per id, zero meaning "no such region"). Ids are below one million, so the join is one array load per record.
3. **Records.** `records.csv` is split right after CRLFs. The generator never writes a CR inside quotes (spec 2.2), so the byte after a CRLF always starts a record and every thread can start parsing without knowing the quote state. Each thread scans 64-byte blocks:
   - AVX2 compares give masks of `"`, `,` and LF; a carry-less multiply by all-ones turns the quote mask into "inside quotes" (a `""` escape flips it twice and changes nothing), and the commas and LFs outside quotes are the record's structure. Each record's four commas and terminator come straight off that mask.
   - The scanner also keeps a running count of quotes, so a text field's unescaped length is its byte length less the delimiting quotes and half of the remaining ones, with no second pass.
   - A byte order mark is skipped only where a record starts, before the id.
   - `id`, `region_id` and `amount` are decoded with a branch-free SWAR routine (eight digits at a time, masked by the known length). Money stays in integer cents.
   - Count, id sum, amount sum/min/max and the byte counts go to the thread's own per-country totals.
4. **Distinct names.** Names are interned by their raw bytes between the quotes (escaping is one to one, so equal raw bytes mean equal values, and the empty quoted and unquoted names are the same key). Each name becomes a 16-byte key: a name of up to 16 bytes is its own bytes, zero padded (exact, as names hold no zero byte); a longer one keeps its first 8 bytes and a 63-bit hash of the rest with the top bit set, so two names would have to share both to be confused. The shared table is one 2 MB array of keys, and a key's slot is the name's id. A lookup is one aligned 16-byte load and compare; a free slot is claimed with a 16-byte compare-and-swap, so readers see a slot empty or whole. Each country has a bitset over name ids; a record sets its bit with a test and an atomic OR. The lookups are pipelined: a record's slot is prefetched when it is parsed, looked up 16 records later (which prefetches its bitset word) and marked 16 records after that. The shared tables are populated by all threads in parallel before the first record.
5. **Output.** Threads count the set bits of their countries; the main thread sums the per-thread totals and prints.

Names of up to 16 bytes are compared exactly. For longer names the tail beyond the first 8 bytes is compared by a 63-bit hash: at full scale about 1,500 of the long names share their first 8 bytes with another, so a wrong merge has a chance of roughly 10^-16.

## Dependencies

None. Standard library and compiler intrinsics only.

## With another 24 hours

- Calibrate laptop timings against board runs, then decide between memory and time trade-offs with real numbers (an earlier version that logged (country, name) pairs instead of atomic marking used 25 MB and was slower).
- Profile the structural scan further: it is about 24 ns per record single-threaded, and the name lookup plus mark still costs about 38 ns per record despite prefetching.
- Trim lines: S is about 374 after `clang-format --style=LLVM` (reference 108).
