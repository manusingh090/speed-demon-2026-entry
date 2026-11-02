# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

One Rust file, `src/main.rs`. Only the 50,000 queries are answered; no full index is built.

1. **Queries.** Every worker parses a part of `queries.txt` and interns its terms into one shared table of 2^15 cache lines (7 tags and 8 actions each) by compare-exchange.
2. **Sample and plan.** Twelve 256 KiB slices of the corpus are sampled. A BOOL term in over 1/32 of the sampled documents gets a bitmap; each phrase's rarest word becomes the word that triggers its check. Each term's action in the table then says what the scan does with it.
3. **Phases.** The corpus is read in phases of about 50 MiB, each starting on a document id divisible by 64. Workers take 256 KiB slices, find token bounds with AVX2 class masks, hash tokens two at a time with AES rounds, and read one table line per token. List terms are written as rows into 64 shards; bitmap terms are gathered as one row of bits per document and transposed into the bitmaps every 64 documents. A phrase is checked when its trigger word appears, on the neighbouring tokens' hashes, and a match is written as a row of a stand-in term.
4. **Postings.** After each phase the shards' rows become sorted, deduplicated 16-bit postings relative to the phase's first document.
5. **Answers.** Each phase answers every query, strictly left to right on lists or bitmaps, into a running result (count, id sum, first ten ids). The answers of one phase run on whichever workers have finished scanning the next. The output is written in one call.

## Dependencies

None. The Rust standard library, `std::arch` intrinsics, and `mmap` and `mallopt` declared by hand.

## With another 24 hours

The scan is still most of the run, and it is now bound by memory accesses (the term table and the per-term state), not by tokenizing: a faster tokenizer on its own cut 35% of tokenizing time single-threaded but almost nothing end to end. Peak memory is now the scan output (36 MB) plus the bitmaps (12 MB) and per-thread scan state; going further toward the 44 MB cap would need a scan output smaller than three bytes per posting. Lines could be trimmed.
