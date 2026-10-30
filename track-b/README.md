# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

The queries are readable before the corpus, so the program reads `queries.txt` first and builds a hash table of every term the queries mention. Tokens that are not in that table are never indexed.

`docs.txt` is memory mapped (over a spare zero page, so wide loads near the end are safe) and split into 12 byte ranges on line boundaries. Each thread first samples the start of its range. At the barrier, terms seen in more than 1/32 of the sampled documents become bitmaps, and all terms are renumbered by sampled frequency, most frequent first, so the scan's per-term state for the common terms shares a few cache lines.

The full scan finds token bounds from one AVX2 character-class mask per 64-byte window, loads each token as 64-bit words (lowercased with one OR, since tokens are only letters and digits), hashes it and prefetches its slot in the term table, and looks it up 8 tokens later so the cache misses overlap. Bitmap terms set a bit. Every other (term, document) pair is filed under a bucket of 512 consecutive term ids, in 1 MB blocks mapped straight from the kernel. The scatter then fills the sorted posting lists bucket by bucket, unmapping each block as it is read, so the scan output and the posting lists are never both fully resident (malloc would have kept the freed blocks). Nothing is sorted: per-thread counts give each thread its own segment of every list.

BOOL queries run strictly left to right. The running result is a sorted list until a bitmap term starts the query or is OR-ed in; from then on it is a bitmap, and count, sum and the ten smallest ids come from popcounts. List against list gallops when one side is at least 16 times smaller, and merges otherwise. PHRASE queries intersect their distinct terms rarest first, then look for the rarest term in each candidate with a 32-byte AVX2 scan for its first letter, and check the neighbouring tokens in place. Queries run on 12 threads into fixed-size slots and are written with one call.

## Dependencies

None. C++20 standard library and POSIX `mmap` only.

## With another 24 hours

The scan is still most of the run, and it is now bound by memory accesses (the term table and the per-term state), not by tokenizing: a faster tokenizer on its own cut 35% of tokenizing time single-threaded but almost nothing end to end. Next would be a smaller posting encoding (doc ids fit in 18 bits) to bring peak memory toward the 44 MB cap, and trimming lines.
