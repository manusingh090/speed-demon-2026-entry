# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++: mmap, 12 threads, records per hour in blocks, per-hour counting sort and nth_element, writer thread | pending | | | |
| 2 | Same design in 222 formatted lines instead of 373 | run-3c1dcbde6115 | 0.0293 | 31.8 | 79.7 |

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
