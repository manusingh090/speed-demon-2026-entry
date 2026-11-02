# Track A: Ingest

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

One C++20 file, `main.cpp`, built as a static binary with `-O3 -march=native`. One pool of 12 threads makes one pass over the log and finishes each hour while the rest of the log is still being parsed; the text is written at the end, once no records are left.

1. **Map and split.** `requests.log` is memory-mapped (input pages are not charged to the container) with a spare page after it, so 64-byte loads near the end are safe. It is cut into 96 line-aligned pieces, claimed in order, guided in size (the first half 1.5 times the average, the next quarter 0.75, the last quarter 0.25) so the threads run out of work close together, and each thread unmaps a piece once it has parsed it.
2. **Parse lines from a bitmap of line ends.** Each 64-byte block's LFs are found with one AVX2 compare and the lines taken from that mask bit by bit, so no line waits on the parse of the one before. A line is read from both ends. From the front: the hour, from the fixed-width timestamp (the 72-hour span is fixed, so the day and hour digits are enough), then the method and the endpoint. From the LF backwards: the latency, the digits after the last space, decoded in one 64-bit word with three multiplies, and the status.
3. **Intern endpoints in one lock-free table.** 8,192 two-byte slots (16 KB, resident in the first-level cache) hold an id plus one, found by a CRC32 of the name's first and last eight bytes. Names are stored 128 bytes apart, each followed by a space as in the log line, so a name of up to 63 bytes and its space are confirmed by the same two 32-byte loads that found its end. A new name is claimed with one compare-and-swap.
4. **File by hour.** Each line becomes a 4-byte record (20-bit latency, 5xx flag, endpoint id plus one, so an unwritten place reads as no record) in its hour's own region, in 4 KB blocks claimed with one atomic add. Latencies of 2^20 - 1 us or more take a second place for the raw value.
5. **Close hours while parsing.** The log is in about clock order, so an hour is closed once the first unparsed piece starts 30 minutes into the next hour (the log's latest lines trail their hour by about ten minutes). Correctness does not depend on that: a thread counts itself into an hour before checking that the hour is open, and a closer marks the hour closed before checking that no thread is in it, so a line for a closed hour is always caught, and the pass is then redone with every hour held open to the end.
6. **Sort each hour outright.** A closed hour's records become 31-bit keys (endpoint rank above the latency) and are radix-sorted in three passes (10, 10 and 11 bits) between the records and the unused rest of the hour's region. Every group comes out sorted at a known place, so the nearest-rank p50, p95 and p99 are read off by index; oversized latencies sort last in their group and are looked up in a short sorted list. The hour's pages go back with one call.
7. **Write the text last.** Each group keeps its six numbers and the exact size of its line (a number's width comes from its bit length). Once every hour is done, the sizes become offsets, the output is mapped at its final size, and every thread formats its hours straight into it, two digits at a time from a table of digit pairs.

## Memory

The box's `/out` is a tmpfs, so the 10 MB summary counts toward M, on top of the container's own start-up (about 5 MiB). A program that does nothing but read the log and write a file that size peaks at about 13.8 MiB. Holding every hour's records until the end of the parse put us at 17.2 to 17.5 MiB; finishing hours during the parse and writing the text only once they are gone brings the peak to about 14.5 MiB.

## Dependencies

None. Standard library and compiler intrinsics only.

## With another 24 hours

- Overlap the text with the end of the parse without raising the peak: write an hour's text early only while the open hours' records leave room under the floor.
- The parse is bound by its own execution (a dry parse costs the same hot or cold): a software-pipelined form that interleaves several lines' table lookups might raise its instructions per cycle.
- S is about 380 lines after `clang-format --style=LLVM` (reference 56); keep trimming where it does not cost readability.
