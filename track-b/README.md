# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

The queries are readable before the corpus, so the program reads `queries.txt` first and builds a hash table of every term the queries mention. Tokens that are not in that table are never indexed.

`docs.txt` is memory mapped and split into 12 byte ranges on line boundaries. Each thread tokenizes its range with a 256-entry lowercase table, looks each token up in the term table and records the distinct query terms of every document. The per-thread counts become write cursors, so a second parallel pass scatters the doc ids into one sorted posting list per term without any sorting.

BOOL queries run strictly left to right on sorted arrays: AND and NOT gallop through the bigger list when one side is at least 16 times smaller, and merge otherwise. PHRASE queries intersect their distinct terms rarest first, then re-tokenize only the surviving candidate documents and look for the terms at consecutive positions. Queries are answered on 12 threads into fixed-size slots and written with one call.

## Dependencies

None. C++20 standard library and POSIX `mmap` only.

## With another 24 hours

Bitmaps for the very common terms, a SIMD tokenizer, and storing positions for phrase anchors so candidates are not re-tokenized.
