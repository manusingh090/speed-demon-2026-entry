# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

Rust, no dependencies (`src/main.rs`). Steps 1 to 15 in PERF.md were C++; the Rust rewrite (step 16) keeps their scan and changes what the scan is asked to do.

## Approach

**Queries first.** `queries.txt` is parsed on 12 threads. Every term then gets an id in one open-addressing table whose slots are split into 64 ranges; each range is filled by one thread, in slot order, so no locks are needed and the probes walk the table almost sequentially. Tokens that are not query terms are never indexed.

**Sample, then decide.** `docs.txt` is memory-mapped (over a spare zero page, so wide loads near the end are safe) and cut into 12 line-aligned ranges. Each thread counts terms over the first 1/32 of its range. From those counts:

- each phrase's rarest term becomes its **anchor**;
- only terms used by BOOL queries get postings; terms in more than 1/32 of documents become bitmaps, the rest lists;
- terms are renumbered: list terms by frequency, dealt round-robin over the 2,048-id buckets of the scan output (so every bucket holds a similar share of postings), then anchors, then terms that appear only in phrases and are never looked up.

**One scan answers every phrase.** Token bounds come from one AVX2 class mask per 64-byte window; each token is loaded as 64-bit words, lowercased with one OR, hashed, and its table slot prefetched, then looked up 8 tokens later so the cache misses overlap. When the token is an anchor, the phrases it anchors are checked right there, comparing the neighbouring tokens byte by byte while the text is in cache, and only the matches (query, document) are kept. Bitmap terms set a bit (atomically only in the two words a thread can share with a neighbour). Every other (term, document) pair is written as one 16-bit unit (term offset in its bucket, gap since the bucket's previous document) into kernel-mapped 64 KB blocks.

**Scatter.** Each thread drains its buckets into its own region of the posting array (16-bit offsets from the thread's first document), unmapping each block as it is read, so the scan output and the lists are never both fully resident.

**Queries.** Phrase answers are the scan's matches. BOOL queries run strictly left to right on sorted lists, galloping from the smaller side, and switch to a bitmap once a bitmap term starts the query or is OR-ed in; count, sum and the ten smallest ids then come from popcounts. Queries are answered 64 at a time on 12 threads and written with one call.

## Dependencies

None. The Rust standard library, plus `mmap`, `munmap`, `prctl` and `mallopt` declared from the C library the standard library already links.

## With another 24 hours

The scan is still most of the run and is bound by memory accesses (the term table and per-term state), not by tokenizing. The next things to try are a small direct-mapped cache for the few hundred most frequent short tokens, and a smaller scan output for the hottest buckets.
