# Track C: Join

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

One C++20 file, `main.cpp`, built as a static binary with `-O3 -march=native` (stripped). Both input files are memory-mapped, with a spare zero page after each so wide loads near the end are safe. Every phase is cut into pieces that the threads of one pool of 12 claim in turn; a phase starts once the one before is done.

1. **Countries.** `regions.csv` is split by lines. Each thread flags the three-letter codes it sees; one thread then numbers them 1..N in code order, which is output order (`_UNMATCHED` is index 0, printed last).
2. **Join table.** Each thread fills its slice of a direct table `region_id -> country index`, one byte per id (zero: no such region). Ids are below one million, so the join is one array load per record.
3. **Records.** `records.csv` is split into 96 pieces right after CRLFs. The generator never writes a CR inside quotes (spec 2.2), so the byte after a CRLF always starts a record, and a thread can parse a piece without knowing the quote state before it.
   - The piece is read a chunk of 64-byte blocks at a time. AVX2 compares give each block's masks of `"`, `,` and LF; a carry-less multiply by all ones turns the quote mask into "inside quotes" (a `""` escape flips it twice and changes nothing). The commas and LFs outside quotes, the separators, are listed for the chunk, and each record takes five of them.
   - A text field's unescaped length is its byte length less its quote count over two, less one if quoted (a quoted field has 2 + 2k quotes for k escapes). Quote counts come from per-block counts and a popcount, so nothing is unescaped.
   - A byte order mark is skipped only where a record starts.
   - `id`, `region_id` and `amount` are decoded together in one AVX2 vector, one 64-bit lane each: the bytes before each number masked off, then digit pairs, quads and halves folded with multiply-adds. Money stays in integer cents.
   - Count, id sum, amount sum, min and max and the byte counts go to the thread's own per-country totals.
4. **Distinct names.** Names are interned by their raw bytes between the quotes (escaping is one to one, so equal raw bytes mean equal values, and the empty quoted and unquoted names are the same key). A name's key is a 64-bit multiply-fold hash of its bytes and length. The shared table has 2^17 slots of 32-bit tags in groups of eight, found with one 32-byte compare; a free slot is claimed with a compare-and-swap, and the slot is the name's id. Each country has a bitset over name ids; a record sets its bit with an atomic OR, and an OR that sets a new bit adds one to the country's distinct names, so no counting pass is needed. Lookups are pipelined: a record's region and name slot are prefetched when it is parsed, it is looked up 16 records later (which prefetches its bitset word for writing) and marked 16 records after that.
5. **Memory layout.** The bitsets (241 rows of 16 KB) and the per-thread totals fill two 2 MB pages almost exactly. On 4 KB pages nearly every mark missed the first-level TLB; on the board this cut the time by 5% at no cost in memory. The region and name tables stay on 4 KB pages: a third 2 MB page would have cost 0.5 MB for 0.5% more speed. Huge pages are refused for the other tables (the box runs them always on), and the threads' stacks are 256 KB, too small for a 2 MB page.
6. **Output.** The main thread sums the per-thread totals in code order and writes the file.

**One accepted risk.** The name table compares 32-bit tags within a group. Two different names would be counted as one only if they share those 32 bits and meet in one group: with 100,000 names that is a chance of about 1 in 10,000 per dataset.

The steps, with the board run of each and the experiments that did not pay, are in [PERF.md](PERF.md).

## Dependencies

None. Standard library and compiler intrinsics only.

## With another 24 hours

- Mark the bitsets without locked operations: give each thread a share of the countries and hand it the (country, name) pairs through per-thread queues. A probe on the board (PERF.md step 38) puts the lock at 4.5% of the time, so this pays only if the queues cost well under that and the extra code stays small.
- Memory: the leaders run under 7.35 MB, where the memory term is capped. That needs denser name ids (3.0 MB of bitsets instead of 3.9) and a packed region table (0.5 MB instead of 1), each of which cost time on the laptop.
