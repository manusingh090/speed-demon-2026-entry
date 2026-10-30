# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

One C++20 file, `main.cpp`, built as a static binary with `-O3 -march=native`.

1. **Map and split.** `requests.log` is memory-mapped (input pages are not charged to the container) with a spare page after it, so 32-byte loads near the end are safe. The file is cut into 12 line-aligned ranges, one per thread.
2. **Parse with fixed offsets and one scan per line.** The timestamp is always 27 bytes and the 72-hour span is fixed, so the hour index comes from the month, day and hour digits with no date library. One 64-byte AVX2 compare finds the spaces and the newline after the timestamp, which gives the method end, the endpoint end and the line end for almost every line. The latency (the digits before the newline) is decoded without a loop: the 16 bytes before the newline, minus `'0'`, with the leading non-digits masked off, are folded 2, 4 and 8 digits at a time with `maddubs`/`madd`.
3. **Intern endpoints per thread.** Each thread has a small open-addressing table keyed by a hash of the endpoint's first and last eight bytes; names stay in the mapping and are compared 32 bytes at a time.
4. **File by hour.** Each line becomes a 6-byte record (`u32` latency, `u16` local endpoint id with the 5xx flag in the top bit) appended to a block list for its hour. Grouping by hour first keeps the next step small.
5. **Merge the endpoint tables once.** All local tables are interned into one global table and sorted, so an endpoint's global id is its rank in unsigned byte order. Groups then come out in output order with no further sorting.
6. **Finish each hour on its own.** Threads claim hours in order. For one hour (about 43k requests) a counting sort by endpoint rank lays the latencies out in an L2-sized scratch array, three `nth_element` calls per group give the nearest-rank p50, p95 and p99, and the lines are formatted. The hour's blocks are freed as soon as they are consumed.
7. **Write while computing.** A writer thread waits on each hour in order and writes it as soon as it is ready, so output overlaps with the remaining hours.

## Dependencies

None. Standard library and compiler intrinsics only.

## With another 24 hours

- Measure page-fault cost of the record blocks on the box and try one large pre-sized arena instead of `malloc` per block.
- Try a two-pass layout (count, then scatter straight into final position) to drop the 2-byte key per record and cut memory.
- Trim lines where it costs nothing, since S above the reference is priced.
