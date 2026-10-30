# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

One C++20 file, `main.cpp`, built as a static binary with `-O3 -march=native`. Plain Dijkstra per query with early exit is the right algorithm for these queries (Dijkstra rank at most 131,071, median 512), so the work went into making each settled node cheap and keeping memory low.

1. **Parse in parallel, place without copies.** `graph.txt` is memory-mapped and split into 12 line-aligned ranges. Pass 1 counts out-degrees with atomic increments (and flags which query targets have an in-edge); pass 2 places every edge's head at its tail's next free slot, giving a CSR array without weights. Self-loops are dropped: they never shorten a path. Parallel edges and duplicates are kept; Dijkstra only ever uses the lightest.
2. **Renumber for locality.** Node ids are a random relabelling, so a search over them misses the cache on almost every node. Each thread cuts the road network into compact blobs of about 512 nodes: it grows a blob by BFS over unclaimed nodes (claimed with a compare-exchange), then starts the next blob from the unclaimed neighbours of the last one's frontier. Consecutive ids are then close on the map, and a search's ball covers a few blobs densely. Nodes no blob reached (islands, isolated nodes) are numbered last.
3. **Build the final graph once.** Pass 3 parses the text again and places every edge (new head id and weight) straight into a CSR array in the new numbering, so the graph is never held twice.
4. **Queries.** Threads claim queries one at a time. `s == t` is 0, and a target with no in-edge is `-1` without searching (otherwise such a query would settle the whole main body). Each search is Dijkstra with a radix heap (keys never drop below the last popped key, so only the lowest non-empty bucket is ever split) and stops when the target is settled. Distances are `u64`: answers can exceed 2^32.
5. **Distances in pages.** Each thread keeps its tentative distances in 4 KB pages handed out on first touch and taken back after every query, so a thread's memory follows the largest search instead of the 1.7M-node graph. Dense per-thread arrays cost 167 MB; pages cost a few MB per thread.

## Dependencies

None. Standard library and POSIX `mmap` only.

## With another 24 hours

- The two placement passes (random atomic writes into the CSR arrays) are now a large share of the build; partitioning edges by tail before placing them would make those writes local.
- Bidirectional search would roughly halve the settled nodes but needs the reverse graph (about 47 MB more); on this scoring formula that is close to break-even.
