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
4. **Distinct names.** Names are interned by their raw bytes between the quotes (escaping is one to one, so equal raw bytes mean equal values, and the empty quoted and unquoted names are the same key). The shared table is lock-free: a 16-bit tag array small enough to stay in L2 is probed first, and each tag guards a 32-byte slot with the id, length and first 24 bytes of the name (longer tails go to an arena). A thread claims a free slot by swapping its tag to BUSY, fills it, then publishes the tag. Ids are dense, so each country has a bitset over name ids; a record sets its bit with a test and an atomic OR. The lookups are pipelined: a record's slot is prefetched when it is parsed, looked up 16 records later (which prefetches its bitset word) and marked 16 records after that.
5. **Output.** Threads count the set bits of their countries; the main thread sums the per-thread totals and prints.

Exact byte comparison is kept everywhere: no hash is trusted as an identity.

## Dependencies

None. Standard library and compiler intrinsics only.

## With another 24 hours

- Calibrate laptop timings against board runs, then decide between memory and time trade-offs with real numbers (an earlier version that logged (country, name) pairs instead of atomic marking used 25 MB and was slower).
- Profile the structural scan further: it is about 24 ns per record single-threaded, and the name lookup plus mark still costs about 38 ns per record despite prefetching.
- Trim lines: S is about 374 after `clang-format --style=LLVM` (reference 108).
