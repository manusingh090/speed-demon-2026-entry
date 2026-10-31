# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | C++: parallel two-pass CSR build over mmap, Dijkstra per query on 12 threads with a binary heap, -1 for targets with no in-edge | pending | | | |
| 2 | Nodes renumbered by level-synchronous parallel BFS, radix heap | pending | | | |
| 3 | BFS in 12 regions without barriers, CSR copied into the new numbering | pending | | | |
| 4 | Compact 512-node blobs chained across the map, distances in recycled 4 KB pages, final CSR placed by a third parse | pending | | | |
| 5 | Weights kept from the second pass; CSR copied into the new numbering in two stages instead of a third parse | pending | | | |
| 6 | malloc mmap threshold pinned, so freed build arrays really go back | pending | | | |

The board app was not installed yet when these were made, so the T and M columns will be filled from board runs. Until then, the local measurements below are the evidence.

## Local measurements

Laptop (Ryzen 7840HS, 8 cores), full seed-1 data in a Docker volume, `--cpus=12`, cgroup `memory.peak`, wall time around the process inside the container. Every row produced the published `distances.txt` hash `a23dbdf0...`.

| Step | Wall (s) | Peak (MB) | Where the time went |
|---|---|---|---|
| 1 | 1.09 to 1.11 | 225 | build 0.13, queries 0.97 (about 480 ns per settled node: random ids miss the cache) |
| 2 | 0.59 to 0.91 | 244 | queries 0.24 to 0.30, but the BFS took 0.1 to 0.27 (thousands of levels, a barrier each) |
| 3 | 0.62 to 0.68 | 246 | BFS 0.04, but queries 0.33 to 0.42: inside 12 huge regions BFS order is ring by ring |
| 4 | 0.45 to 0.52 | 82 | queries 0.22 to 0.24, placement passes about 0.06 to 0.15 each |
| 5 | 0.43 vs 0.50 for step 4 (same session) | 102 | third parse with random atomic placement replaced by a parallel copy |
| 6 | 0.44, same as 5 | 85 | the 17 MB were freed vectors glibc kept |
| 7 | Build arrays allocated with calloc (lazily zeroed by the kernel, first touched by the threads that fill them) instead of vectors zero-filled on one thread; new ids and per-node degrees in the new numbering filled in parallel | run-f9dd6c762386 | 0.360 | 102 | 60.53 |
| 8 | Edges packed into one 32-bit word (head in the low 21 bits, weight above it, heavy weights in a sorted side list) in both numberings; transparent huge pages off | run-b8d2e5b996d8 | 0.346 | 64 | 62.76 |
| 9 | Bidirectional Dijkstra over the packed forward graph and a packed reverse graph built in place; query ends mapped up front so the id map is freed first | run-9ea1a9d51d35 | 0.240 | 71 | 65.84 |
| 10 | Count and fill passes in batches of 32 edges with every random line prefetched first; BFS and renumbering prefetch the nodes coming up | run-e72d02d00206 | 0.229 | 72 | 65.95 |
| 11 | Search constants copied into the thread so the loop keeps them in registers; a queued node's offsets prefetched | run-23727a1a2adb | 0.228 | 73 | 65.96 |
| 12 | Rewritten in Rust with a new build: one parse filing edges by tail range, each range's adjacency built in cache; in-edge check read off the reverse graph | run-18295702ad2c | 0.185 | 71 | 67.59 |
| 13 | Numbers parsed with one 8-byte load each; Rust trimmed from 494 to 478 rustfmt lines | pending | | | |

## What the profile said, step by step

**Step 1 to 2.** The queries were 90% of the run at about 480 ns per settled node, which is cache misses: ids are a random relabelling, so every neighbour, offset and distance is somewhere else in memory. Renumbering in BFS order cut the query phase by about 3.5x.

**Step 2 to 3.** A road network has thousands of BFS levels, and a level-synchronous BFS pays a barrier for each one. Letting each thread grow its own region removed the barriers, but the queries got slower again: within a region of 145,000 nodes the order is ring by ring, so a search's ball spans many thin rings and uses few nodes per cache line.

**Step 3 to 4.** Small blobs (512 nodes, grown by BFS, chained by starting each blob from the last one's frontier) give 2-D locality. Peak memory was the per-thread distance arrays (12 x 14 MB); pages of 512 distances handed out on first touch and recycled per query brought the peak from 246 to 82 MB. The CSR copy in step 3 also held the graph twice (116 MB); building the final CSR by a third parse instead keeps one copy.

**Step 4 to 6.** The third parse placed every edge with a random atomic increment and a random write, about 0.11 to 0.15 s. Keeping the weights from the second pass (as a separate array next to the heads) and copying both into the new numbering in parallel is faster, and copying weights first, then heads, freeing each old array straight after, keeps the peak at about one and a half graphs. The measured peak was still 17 MB above that: glibc raises its mmap threshold after the first large free, so later large vectors came from the heap and stayed resident when freed. Pinning the threshold with `mallopt` fixed it.

**Step 6 to 7.** Phase timers: count 0.04 s, fill 0.10, order 0.045, renumber 0.075, search 0.19. About 110 MB of build arrays (offsets, heads, weights, both renumbered copies, ids) were value-initialised by `std::vector`, a single-threaded memset that also took every page fault on one thread, before parallel passes overwrote them. With `calloc` (glibc skips the memset for fresh mmapped blocks) the zeroing happens lazily, spread over the threads that first write each page. The two serial loops that assign new ids from each thread's blob chain and scatter degrees into the new numbering now run in parallel. Locally 385 ms against 428 ms for step 6 over 9 alternating runs, same 84 MB peak and hash on all four datasets. Formatted code lines 277 to 290.

Also measured and kept out at this step: dense per-thread distance arrays (no faster, 236 MB), and heads and weights interleaved into one array (no faster within noise, 97 MB).

**Step 7 on the board.** run-f9dd6c762386: 0.360 s but 102 MiB against 83 MiB for step 6, so K fell from 16.79 to 16.24. Locally the peak did not move (84 MB), even with glibc asked for huge pages, so the box's transparent huge pages are the likely cause; step 8 turns them off for this process, as Tracks A, B and C already do.

**Step 7 to 8.** Search costs about 85 ns per settled node, and two cheaper experiments showed the queue is not the limit: Dial's bucket queue (an 8,192-slot ring, which holds every main-body weight; the largest is under 3,500) was no faster and cost 9 MB, and 6 or 8 search threads were slower than 12. What the search waits on is memory, so the edges got smaller. Each edge is now one word: the head's id in the low bits (21 for 1.74M nodes) and the weight in the 11 bits above. A weight of 2,047 or more (about 0.1% of edges, plus the injected ones near 2^32) is stored as all ones, and its value sits in a list sorted by edge position, found by binary search. The fill pass writes that word in the old numbering (one random write per edge instead of two), the BFS reads heads through a mask, and the renumbering is one pass that maps the head and keeps the weight bits. Peak was the old heads and weights alive next to the new arrays; now it is one 20 MB array next to another. Locally (9 alternating runs): 359 ms and 65 MB against 402 ms and 84 MB. Same hash on full, sample, tiny, the worked example (which has 2^32 - 1 weights and an answer above 2^32) and a seed-7 sample checked against the reference.

**Step 8 to 9.** `perf` (in a privileged throwaway container, since the Docker VM's kernel exposes the counters) split the CPU: search 56%, fill pass 17.5%, blob ordering 10.7%, renumbering 6.4%, count pass 4.5%. The leader's whole run fits in about 1.5 s of CPU on 12 threads; our searches alone took 2.2 s, about 95 ns per settled node. `perf annotate` put the two hottest instructions (about 26% between them) on the loads of the edge word and of the tentative distance: real cache misses, so the way down was fewer settled nodes, not cheaper ones. Bidirectional search was ruled out at step 3 because the reverse graph cost 40 MB; with packed edges it costs 20 MB plus offsets. The reverse graph is counted into `rfirst[v + 1]`, prefix-summed, filled using `rfirst[v]` as the cursor (which leaves it at v's end), then shifted up by one, so no second cursor array is needed. Each query grows the side with the smaller queue, records the best length where the sides meet, and stops once the two smallest queued keys add up to at least that length; a side that runs dry ends the search. Distances are read across sides with a lookup that does not hand out a page. The annotate also showed the relaxation loop reloading the shift, mask and edge pointer through the lambda's captures on every edge; they are locals now. Locally (9 alternating runs): 279 ms against 367 ms, peak 73 MB against 65. Same hash on full, sample, tiny, the worked example, seeds 7 and 13 at sample scale and seed 21 at tiny scale, each checked against the reference.

**Step 9 on the board.** run-9ea1a9d51d35: 0.240 s against 0.346 s, 71 MiB, K 20.74, 65.84 points.

**Step 9 to 10.** With the search halved, `perf` put the build ahead of it: the fill pass 25.5% of CPU (about 110 ns per edge), blob ordering 17%, renumbering 9%, the count pass 7%. The fill pass's cost is a property of x86: an atomic add waits for every store before it, so with one edge at a time each edge's cache miss (the random cursor, then the random slot) waited for the previous one's. Edges now go through in batches of 32: parse 32, prefetch all 32 cursors, take the 32 slots (now cache hits) while prefetching the 32 destinations, then write. The count pass does the same for its cursors and the in-edge flags. The BFS knows its queue ahead, so it fetches node offsets 8 entries ahead and edge lists 4 ahead; the renumbering walks old ids in order, so it fetches the new offsets and the heads' new ids 8 nodes ahead and the destination slots 4 ahead. Locally user CPU fell from 1.5 to 2.0 s to 1.2 to 1.35 s and wall time from about 0.26 s to about 0.18 to 0.24 s, same 73 MB; the fill pass is down to about 11% of CPU and the renumbering to 5.5%. Same hash on full, sample, tiny, the worked example and seeds 7, 13 and 21.

**Step 10 on the board.** run-e72d02d00206: 0.229 s against 0.240 s, but the extra code took the formatted size from 362 to 400 lines, so K moved only from 20.74 to 20.84. In the same run Track A's unchanged code measured 0.039 s against its usual 0.031 s: the box's run-to-run spread is as large as most single steps.

**Step 10 to 11.** The annotated search still reloaded the mask, shift and heavy-weight marker through the thread lambda's by-reference captures on every edge; they are copies inside the thread now. A node's offsets are prefetched when it is queued. The laptop could not separate this from noise (user CPU 1.4 to 1.7 s for both); it removes three loads per edge, so it stays, and the board run is the measurement. Same hash on all seven datasets.

**After step 11: what was measured and not kept.** Wall-clock phase timers (local, ms): count pass about 30 to 40, fill 35 to 45, blob ordering about 35, new ids 9, renumbering 18, reverse graph 15, searches 60 to 75. Per-thread timers on the search showed all twelve threads finishing within about 5 ms of each other, so there is no tail of large queries to rebalance. Three ideas for the build did not separate from noise and were dropped: parsing the text once and keeping (tail, word) pairs for the fill pass (225 vs 223 ms, 3 MB more), pre-faulting each thread's slice of the input file with `MADV_POPULATE_READ`, and pre-faulting each thread's slice of the cursor and link arrays with `MADV_POPULATE_WRITE` (fill pass perhaps 3 ms faster, inside the spread). The laptop's run-to-run spread is about 15% and the box's up to 25%, so steps worth 5% or less can no longer be told apart; the next step for D has to be a larger structural change.

**Step 11 to 12: Rust, and a different build.** The C++ build parsed the text twice (count, then fill) with a random atomic add per edge each time, about 75 ms of the run. The Rust version parses once: each thread appends every edge to one of about 27 lists by its tail's node range (sequential writes, 27 streams per thread), then each range is built on its own inside a 256 KB counter array. Phase timers: parse plus build 58 to 80 ms against 93 to 133 ms for the C++ count and fill passes in the same session. The first Rust build was only 4% faster overall because its search ran slower (120 to 140 ms against 90 to 130): bounds checks in the hottest loop. With unchecked access in the search, distance pages and heap buckets, it runs 12 to 15% faster than the C++ (local median 242 ms against 287, `perf stat -r 8` 0.237 to 0.246 s against 0.270 to 0.274), same 73 MB peak. Trimmed under `rustfmt` defaults from 523 to 494 lines (502 with the Dockerfile, against 401 counted for the C++): about 3% off K for size against about 8% on for time. Same hash on full, sample, tiny, the worked example and seeds 7, 13 and 21.

**Step 12 on the board.** run-18295702ad2c: 0.185 s against 0.226 s for the C++, 71 MiB, 505 counted lines against 401; K 21.0 to 22.49, points 66.16 to 67.59. The box gained more than the laptop predicted (18% against 12 to 15%).

**Step 12 to 13.** Phase timers (medians of 9, ms): parse 35, part build 24, blob order 22, new ids 6, renumbering 25, reverse graph 18, searches 106. The search matched the C++ one within about 10% in an interleaved run, so the parse was the cleaner target: it read 92 MB a byte at a time. Digits have bit 0x10 set and the space and newline that end a number do not, so one 8-byte load finds the end of any number of up to 7 digits (every node id; weights of 8 or more digits take the byte loop), and the digits combine in three multiply steps. The 8-byte reads needed the file mapped over a spare zero page, as the C++ had it. Parse median 40 to 32 ms. Then the source was trimmed under `rustfmt` from 507 to 478 lines without changing behaviour: one-line forms rustfmt keeps (a short if/else choosing the search side, `saturating_add` instead of an INF check, a direct pointer write instead of a setter), part sizes summed before the buckets move, and one atomic pointer for both reverse-graph passes. With the trims the run is about 5% fewer instructions (`perf stat -r 8`: 0.233 s against 0.240 s), peak 72 MB. Same hash on all seven datasets.

## Tried and kept out

- Level-synchronous parallel BFS (step 2): good order, too many barriers.
- One large BFS region per thread (step 3): fast to build, poor locality.
- Copying the whole CSR at once into the new numbering (step 3): 116 MB peak; the staged copy in step 5 gets the speed at 85 MB.
- Blob sizes 128 and 2048 instead of 512: query time the same within noise.
- Bidirectional search: would roughly halve settled nodes, but the reverse graph adds about 47 MB; on this formula the memory cost outweighs the time saved.

## Board runs

Runs measured on the scoring box after the team was registered. M is the box's cgroup peak in MiB; K and points as the board reports them.

| Commit | Run ID | T (s) | M (MiB) | K | Points | What it measured |
|---|---|---|---|---|---|---|
| 8041c6b | run-e87fa4aef6e8 | 0.3766 | 79.6 | 16.62 | 61.04 | Steps 5 and 6 (staged copy, malloc threshold) |
| b4606d2 | run-fa7637b07d93 | 0.3823 | 83.4 | 16.51 | 60.89 | Same Track D code |
| c451b56 | run-66c30a95d45a | 0.3787 | 83.8 | 16.57 | 60.97 | Same Track D code |
| 5a6463e | run-b2302fb71532 | 0.3722 | 113.8 | 15.57 | 59.62 | One parse with edges filed per owner instead of the second parse: no faster on the box and 30 MiB more; reverted |
| 836c19e | run-f9dd6c762386 | 0.3599 | 102 | 16.24 | 60.53 | Step 7: calloc build arrays; faster, but 19 MiB more with huge pages on |
| 33a36ec | run-b8d2e5b996d8 | 0.3456 | 64 | 18.00 | 62.76 | Step 8: packed edges, THP off |
