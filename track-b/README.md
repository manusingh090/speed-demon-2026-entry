# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

The queries are readable before the corpus, so the program reads `queries.txt` first and builds a hash table of every term the queries mention, on 12 threads (an empty slot is claimed with a compare-and-swap). Tokens that are not in that table are never indexed.

`docs.txt` is memory mapped (over a spare zero page, so wide loads near the end are safe) and split into 12 byte ranges on line boundaries. Each thread first samples the start of its range. At the barrier, terms seen in more than 1/32 of the sampled documents become bitmaps, and the other terms are dealt out by sampled frequency, round-robin over the buckets of the scan output, so every bucket carries a similar share of the postings and the per-term state of the common terms shares a few cache lines at each bucket's start.

The full scan finds token bounds from one AVX2 character-class mask per 64-byte window, loads each token as 64-bit words (lowercased with one OR, since tokens are only letters and digits), hashes it and prefetches its slot in the term table, and looks it up 8 tokens later so the cache misses overlap. Bitmap terms are numbered first; each thread collects a bitmap term's bits for the current 64 documents in one word and ORs it into the shared bitmap when it moves on. Every other (term, document) pair is filed under a bucket of 2,048 consecutive term ids, in blocks mapped straight from the kernel, as one 16-bit unit: the term's offset in its bucket and the gap since the bucket's previous document (an escape unit holds larger gaps). After the scan all threads lay out the posting lists together: each term's postings are one stretch, a segment per thread, with the 12 segment lengths stored side by side. Each thread then scatters its own scan output bucket by bucket, unmapping each block as it is read, so the scan output and the posting lists are never both fully resident (malloc would have kept the freed blocks). A posting is the document's 16-bit offset from its thread's first document; a query decodes a list term's one stretch into a scratch array.

BOOL queries run strictly left to right. The running result is a sorted list until a bitmap term starts the query or is OR-ed in; from then on it is a bitmap, and count, sum and the ten smallest ids come from popcounts. List against list gallops when one side is at least 16 times smaller, and merges otherwise. Each phrase's rarest term in the sample (if it is in under 1/200 of documents) is its anchor: anchors are numbered after all other terms, so the scan tells them apart by id, and at every occurrence of an anchor the scan checks that anchor's phrases in place, reading the neighbouring tokens back and forward up to the document's TAB and LF while they are in cache. Matches are kept as (query, document) pairs; anchors also keep ordinary lists for BOOL queries. Phrases without an anchor intersect their distinct terms rarest first, then look for the rarest term in each candidate with a 32-byte AVX2 scan for its first letter. Queries run on 12 threads, 64 at a time, each chunk into its own buffer.

## Dependencies

None. C++20 standard library and POSIX `mmap` only.

## With another 24 hours

The scan is still most of the run, and it is now bound by memory accesses (the term table and the per-term state), not by tokenizing: a faster tokenizer on its own cut 35% of tokenizing time single-threaded but almost nothing end to end. Peak memory is now the scan output (36 MB) plus the bitmaps (12 MB) and per-thread scan state; going further toward the 44 MB cap would need a scan output smaller than three bytes per posting. Lines could be trimmed.
