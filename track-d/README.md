# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

One Rust file, `src/main.rs`, with no crates (the C++ version it replaced is in the git history up to commit ee086ce). Plain Dijkstra with early exit is the right algorithm for these queries (Dijkstra rank at most 131,071, median 512), run from both ends, so the work went into a cheap build and cheap settled nodes.

1. **Parse once.** `graph.txt` is memory-mapped and cut into one line-aligned range per thread. Each thread files every edge under the part of the node range its tail falls in (about 107 parts of 16,384 nodes), as sequential appends into 1,024-record chunks carved from one arena per thread; a record is 6 bytes (a 16-bit offset in the part and the edge word). An edge is one 32-bit word, the head in the low bits and the weight above it; a weight too large for its field is stored as all ones with its value on a side list.
2. **Build each part on its own.** A thread takes a part, counts its nodes' out-degrees and places its edges, all inside a 64 KB counter array. This replaced two passes of random atomic adds over the whole graph. Every phase runs on one pool of threads started once.
3. **Renumber for locality.** Node ids are a random relabelling, so each thread cuts the road network into compact blobs of 512 nodes grown by BFS (nodes claimed by compare-exchange), and the graph is copied into that numbering, then reversed in place.
4. **Bidirectional Dijkstra.** Each query grows the side with the smaller queue, records the best length where the sides meet and stops once the two smallest queued keys reach it. 4-ary heaps; distances in 4 KB pages handed out on first touch. A target with no in-edge is -1 at once, read off the reverse graph.

## Dependencies

None. Standard library and POSIX `mmap` only.

## With another 24 hours

- The second pass (random atomic writes into the CSR arrays) is now the largest part of the build; partitioning edges by tail before placing them would make those writes local.
- Bidirectional search would roughly halve the settled nodes but needs the reverse graph (about 47 MB more); on this scoring formula that is close to break-even.
