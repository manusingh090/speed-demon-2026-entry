# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

The queries are readable before the corpus, so the program reads `queries.txt` first and builds a hash table of every term the queries mention. Tokens that are not in that table are never indexed.

`docs.txt` is memory mapped and split into 12 byte ranges on line boundaries. Each thread first samples the start of its range, and terms seen in more than 1/32 of the sampled documents are stored as bitmaps. The full scan then tokenizes with a 256-entry table, loads each token as 64-bit words (lowercased with one OR, since tokens are only letters and digits) and looks it up in the term table. Bitmap terms set a bit; the other terms are written out per document and scattered into sorted posting lists using the per-thread counts, so nothing is sorted.

BOOL queries run strictly left to right. The running result is a sorted list until a bitmap term starts the query or is OR-ed in; from then on it is a bitmap, and count, sum and the ten smallest ids come from popcounts. List against list gallops when one side is at least 16 times smaller, and merges otherwise. PHRASE queries intersect their distinct terms rarest first, then look for the rarest term in each candidate with a 32-byte AVX2 scan for its first letter, and check the neighbouring tokens in place. Queries run on 12 threads into fixed-size slots and are written with one call.

## Dependencies

None. C++20 standard library and POSIX `mmap` only.

## With another 24 hours

A SIMD tokenizer for the scan, which is now most of the run, and a smaller encoding for the scan's per-document term lists, which set peak memory.
