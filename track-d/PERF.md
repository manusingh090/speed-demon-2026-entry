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

## What the profile said, step by step

**Step 1 to 2.** The queries were 90% of the run at about 480 ns per settled node, which is cache misses: ids are a random relabelling, so every neighbour, offset and distance is somewhere else in memory. Renumbering in BFS order cut the query phase by about 3.5x.

**Step 2 to 3.** A road network has thousands of BFS levels, and a level-synchronous BFS pays a barrier for each one. Letting each thread grow its own region removed the barriers, but the queries got slower again: within a region of 145,000 nodes the order is ring by ring, so a search's ball spans many thin rings and uses few nodes per cache line.

**Step 3 to 4.** Small blobs (512 nodes, grown by BFS, chained by starting each blob from the last one's frontier) give 2-D locality. Peak memory was the per-thread distance arrays (12 x 14 MB); pages of 512 distances handed out on first touch and recycled per query brought the peak from 246 to 82 MB. The CSR copy in step 3 also held the graph twice (116 MB); building the final CSR by a third parse instead keeps one copy.

**Step 4 to 6.** The third parse placed every edge with a random atomic increment and a random write, about 0.11 to 0.15 s. Keeping the weights from the second pass (as a separate array next to the heads) and copying both into the new numbering in parallel is faster, and copying weights first, then heads, freeing each old array straight after, keeps the peak at about one and a half graphs. The measured peak was still 17 MB above that: glibc raises its mmap threshold after the first large free, so later large vectors came from the heap and stayed resident when freed. Pinning the threshold with `mallopt` fixed it.

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
