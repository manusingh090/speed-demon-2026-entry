# Track D: Traverse

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

One Rust file, `src/main.rs`, with no crates (the C++ version it replaced is in the git history up to commit ee086ce). Plain Dijkstra with early exit is the right algorithm for these queries (Dijkstra rank at most 131,071, median 512), run from both ends, so the work went into a cheap build and cheap settled nodes.

1. **Parse once.** `graph.txt` is memory-mapped and cut into one line-aligned range per thread. Each thread files every edge under the part of the node range its tail falls in (about 107 parts of 16,384 nodes), as sequential appends into 1,024-record chunks carved from one arena per thread; a record is 6 bytes (a 16-bit offset in the part and the edge word). An edge is one 32-bit word, the head in the low bits and the weight above it; a weight too large for its field is stored as all ones with its value on a side list.
2. **Build each part on its own.** A thread takes a part, counts its nodes' out-degrees and places its edges, all inside a 64 KB counter array. This replaced two passes of random atomic adds over the whole graph. Every phase runs on one pool of threads started once.
3. **Renumber for locality.** Node ids are a random relabelling, so each thread cuts the road network into compact blobs of 512 nodes grown by BFS (nodes claimed by compare-exchange). As it orders a node it appends the node's edges to its own region of the new edge array (one zeroed reservation, region t at t times m, so only written pages count), which copies the graph into the new order in sequential writes; new ids are padded per thread to whole groups of 8 so packed offsets never span two regions. One pass then maps the heads to new ids and counts the in-degrees, and the reverse graph is placed from that.
4. **Bidirectional Dijkstra.** Each query grows the side with the smaller queue, records the best length where the sides meet and stops once the two smallest queued keys reach it. Each side's queue is a circular bucket queue (Dial's algorithm): edge weights are small integers, so a key less than 4,096 above the current minimum goes straight into its bucket and a bitmap finds the next nonempty one, where a heap's pop sifted through every level (it had been 45% of the search); the few injected heavy edges wait in a short list. Distances in pages handed out on first touch, stored as u32, which halves the memory a search touches. A length that does not fit is dropped and noted; if the answer then comes out at 2^32 - 1 or more, the query runs again with u64 distances. A target with no in-edge is -1 at once, read off the reverse graph.
5. **Packed offsets.** Each graph's edge offsets are a u32 per group of 8 nodes plus a byte per node (plain u32s if a group ever has more than 255 edges), 2.6 MB instead of 7 MB per graph.
6. **A short exit.** Each thread drops its share of graph.txt's pages (`MADV_DONTNEED`) once it runs out of queries, and after the searches every thread hands its share of the graph's pages back the same way, so the exit no longer frees about 140 MB of mappings on one thread.

## Dependencies

None. Standard library and POSIX `mmap` only.

## With another 24 hours

- The second pass (random atomic writes into the CSR arrays) is now the largest part of the build; partitioning edges by tail before placing them would make those writes local.
- Bidirectional search would roughly halve the settled nodes but needs the reverse graph (about 47 MB more); on this scoring formula that is close to break-even.
