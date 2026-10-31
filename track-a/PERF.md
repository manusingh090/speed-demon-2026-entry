# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++: mmap, 12 threads, records per hour in blocks, per-hour counting sort and nth_element, writer thread | pending | | | |
| 2 | Same design in 222 formatted lines instead of 373 | run-3c1dcbde6115 | 0.0293 | 31.8 | 79.7 |
| 3 | One thread pool with a barrier; each hour written with pwrite at an offset handed on by the hour before, instead of per-hour strings and a writer thread; chunks released from the front of each region in 256 KB batches instead of one madvise per chunk; THP off | run-ab09a84f93b8 | 0.0310 | 20 | 80.64 |
| 4 | Hours computed into compact per-group numbers with their exact text size, then formatted straight into the mapped output file at known offsets; endpoint names compared with one 32-byte SIMD compare | run-b962d6b976eb | 0.0301 | 20 | 80.47 |

## Local measurements

Laptop (Ryzen 7840HS), full seed-1 data in a Docker volume, `--cpus=12`, cgroup `memory.peak`, wall time around the process inside the container. Every version produced the published hash `495ca096...`.

| Version | Wall (ms, median) | Peak (MB) | Where the time went |
|---|---|---|---|
| v1: one global (hour x endpoint) grid, count, prefix and scatter over it | 74 to 87 | 48 | parse 30, serial prefix over 132k groups x 12 threads 13, scatter 13, format 9, write 6 |
| v2: records filed per hour; each hour sorted and formatted on its own in L2 | 41 to 46 | 33 | parse 21 to 38, hours 9 |
| v3: one 64-byte SIMD scan per line, loop-free latency decode | 39 to 42 | 33 | within noise of v2 |
| step 2: v3 rewritten shorter (to_chars, fwrite, fixed endpoint table) | 38 to 46 | 33 | same speed, S 373 to 222 |

## What the profile said

**v1 to v2.** More than half the time went to a global grid of 132,624 (hour, endpoint) groups: a serial prefix sum across 12 per-thread count arrays and a scatter with random writes. Filing records per hour while parsing makes each hour's work (about 43k latencies) small enough for L2: a counting sort by endpoint rank, three `nth_element` calls per group, formatting, and the writer thread emits hours in order as they finish.

**v2 to v3.** Page faults on the input were 3 to 7 ms (a variant that only touched each page); `MAP_POPULATE` was slower (serial, +15 ms). The rest was per-line parsing, so the method, endpoint and line ends now come from one 64-byte compare. The gain was inside laptop noise.

**Step 2.** S is counted after `clang-format --style=LLVM`, which wraps at 80 columns. Folding the per-hour summary into `main`, using `std::to_chars` and `fwrite`, and a fixed-size endpoint table (a seed has 1,842 endpoints) took it from 373 to 222 lines with no change in speed.

**Step 3.** Phase timers showed the hour phase at about 17 ms for work that is only a counting sort and small selections. Dropping the per-chunk `madvise` (about 1,600 calls, each a TLB shootdown across every thread) cut it to about 12 ms, but peak memory went from 23 to 32 MB: the finished hours' strings piled up in malloc arenas, and the output file's own pages count toward the cgroup as it is written. So the strings went: each thread formats an hour into one reusable buffer, takes its file offset from the hour before, passes the next offset on and writes with `pwrite`. Chunks still have to go back while the output grows, but the log is in arrival order, so the front of each thread's region empties hour by hour; one `madvise` per 256 KB of finished chunks does it. Transparent huge pages are off, as on B and C, so a part-written region is not backed by 2 MB pages on the box. Locally (11 alternating runs): median 40 ms and 21 MB, against 44 ms and 23 MB for step 2, same hash on full, sample, tiny and the worked example. Formatted code lines 245 to 254.

**Step 3 to 4.** `perf` (in a privileged throwaway container) put `memcmp` at 12% of all cycles, one call per line to confirm the endpoint lookup, and a kernel lock (`osq_lock`, `rwsem_spin_on_owner`) at 6 to 14%, which its call graph traced to `pwrite`: the filesystem takes the file's lock for every write, so twelve threads writing one file queue on it. Both names live in the log, where 32 bytes can always be read, so names of up to 32 bytes are now compared in one AVX2 step. Per-step timers inside the hour phase (CPU summed over threads, ms): counting 13 to 22, scattering 6 to 8, release 2, selection and formatting 50 to 54, waiting for the previous hour's offset 24 to 60, `pwrite` 21 to 31. About 40% of the phase was waiting and writing. The hours now run in two steps around a barrier. First each thread computes its hours' groups (24 bytes each, about 3 MB in all) and the exact size each hour's text will take; a number's width comes from its bit length (checked against `len(str(x))` on half a million values and every power-of-ten boundary). Then the last thread in turns sizes into offsets, sets the file's length and maps it (`MAP_SHARED`, so the file is opened read-write), and every thread formats its hours straight into it. No hour waits for another, nothing queues on the file's lock, and the output's pages fill only after the records are released. Locally (15 alternating runs): median 32 ms against 38 ms, peak 20 against 21 MB. Same hash on full, sample, tiny, the worked example, a seed-9 sample and an empty log (zero-byte output), all through `check.py`'s bind-mounted `/out`. Formatted code lines 264 to 294.

**Step 4 on the board.** run-b962d6b976eb: 0.0301 s against 0.0310 s, same 20 MiB, but the source grew from 261 to 301 counted lines, so K went from 41.02 to 40.67. The time gain (3%) is worth less on this formula than the 15% more code costs; the next step is to win those lines back. An averaged profile after this step still shows `memcmp` at 7% (endpoint names longer than 32 bytes, and the name sort in the merge).

## Tried and kept out

- `MAP_POPULATE`: slower, the populate is serial.
- A plain digit loop instead of the SIMD latency decoder: the laptop could not tell them apart; kept SIMD.

## Board runs

Runs measured on the scoring box after the team was registered. M is the box's cgroup peak in MiB; K and points as the board reports them.

| Commit | Run ID | T (s) | M (MiB) | K | Points | What it measured |
|---|---|---|---|---|---|---|
| ac44ba2 | run-3c1dcbde6115 | 0.0293 | 31.8 | 39.34 | 79.74 | Trimmed version (step 2) first measured on the board |
| 8041c6b | run-e7a6c67e6934 | 0.0329 | 34.4 | 36.55 | 78.15 | Same Track A code; box variance between runs |
| b4606d2 | run-cae4bc93c3f8 | 0.0294 | 33.5 | 38.87 | 79.48 | Same Track A code |
| c451b56 | run-95a7235e8965 | 0.0295 | 34.2 | 38.61 | 79.34 | Same Track A code |
| 5a6463e | run-18cf1fb4a517 | 0.0304 | 32.8 | 38.36 | 79.19 | Same Track A code |
| 836c19e | run-ab09a84f93b8 | 0.0310 | 20 | 40.99 | 80.64 | Step 3: pwrite per hour, batched chunk release, THP off |
