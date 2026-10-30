# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 72.69 | 893 | 0 |
| 1 | C++: parallel two-pass CSR build over mmap, Dijkstra per query on 12 threads with a binary heap, -1 for targets with no in-edge | pending | | | |
| 2 | Nodes renumbered by level-synchronous parallel BFS, radix heap | pending | | | |
| 3 | BFS in 12 regions without barriers, CSR copied into the new numbering | pending | | | |
| 4 | Compact 512-node blobs chained across the map, distances in recycled 4 KB pages, final CSR placed by a third parse | pending | | | |

The board app was not installed yet when these were made, so the T and M columns will be filled from board runs. Until then, the local measurements below are the evidence.

## Local measurements

Laptop (Ryzen 7840HS, 8 cores), full seed-1 data in a Docker volume, `--cpus=12`, cgroup `memory.peak`, wall time around the process inside the container. Every row produced the published `distances.txt` hash `a23dbdf0...`.

| Step | Wall (s) | Peak (MB) | Where the time went |
|---|---|---|---|
| 1 | 1.09 to 1.11 | 225 | build 0.13, queries 0.97 (about 480 ns per settled node: random ids miss the cache) |
| 2 | 0.59 to 0.91 | 244 | queries 0.24 to 0.30, but the BFS took 0.1 to 0.27 (thousands of levels, a barrier each) |
| 3 | 0.62 to 0.68 | 246 | BFS 0.04, but queries 0.33 to 0.42: inside 12 huge regions BFS order is ring by ring |
| 4 | 0.45 to 0.52 | 82 | queries 0.22 to 0.24, placement passes about 0.06 to 0.15 each |

## What the profile said, step by step

**Step 1 to 2.** The queries were 90% of the run at about 480 ns per settled node, which is cache misses: ids are a random relabelling, so every neighbour, offset and distance is somewhere else in memory. Renumbering in BFS order cut the query phase by about 3.5x.

**Step 2 to 3.** A road network has thousands of BFS levels, and a level-synchronous BFS pays a barrier for each one. Letting each thread grow its own region removed the barriers, but the queries got slower again: within a region of 145,000 nodes the order is ring by ring, so a search's ball spans many thin rings and uses few nodes per cache line.

**Step 3 to 4.** Small blobs (512 nodes, grown by BFS, chained by starting each blob from the last one's frontier) give 2-D locality. Peak memory was the per-thread distance arrays (12 x 14 MB); pages of 512 distances handed out on first touch and recycled per query brought the peak from 246 to 82 MB. The CSR copy in step 3 also held the graph twice (116 MB); building the final CSR by a third parse instead keeps one copy.

## Tried and kept out

- Level-synchronous parallel BFS (step 2): good order, too many barriers.
- One large BFS region per thread (step 3): fast to build, poor locality.
- Copying the CSR into the new numbering instead of parsing a third time: about 65 ms faster, 116 MB peak instead of 65 MB; on this formula a wash, so the smaller one stays.
