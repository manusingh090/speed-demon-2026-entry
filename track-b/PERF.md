# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | C++ rewrite: read queries first, index only their terms, 12-thread tokenize over mmap, galloping set ops, phrase check on candidates only | pending | | | |
| 2 | Phrase check compares bytes instead of re-tokenizing and hashing candidates | pending | | | |
| 3 | Terms in over 1/32 of docs become bitmaps, chosen from a 1/32 sample | pending | | | |
| 4 | Keys loaded 8 bytes at a time, cheaper hash, table load 1/2 | pending | | | |
| 5 | AVX2 scan for the phrase anchor's first letter | pending | | | |
| 6 | BOOL results stay a bitmap once a bitmap term is OR-ed in | pending | | | |
| 7 | SIMD class-mask tokenizer, term-table slot prefetched 8 tokens ahead | pending | | | |
| 8 | Scan output bucketed by term range in kernel-mapped blocks, scattered bucket by bucket | pending | | | |
| 9 | Queries stored flat instead of two vectors each | pending | | | |
| 10 | Terms renumbered by sampled frequency; tokenizer keeps one mask per 64-byte window | pending | | | |

The board app was not installed yet when these were made, so the T and M columns will be filled from board runs. Until then, the local measurements below are the evidence.

## Local measurements

Laptop, 16 threads with turbo, full seed-1 data in a Docker volume, `--cpus=12`, cgroup `memory.peak`. Every row produced the published `results.txt` hash `f86973c2...`. Timer is wall time around the process inside the container.

| Step | Wall (s) | Peak (MB) | Where the time went |
|---|---|---|---|
| 1 | 0.89 to 1.15 | 195 | parse 0.04, index 0.45 to 0.7, queries 0.45 |
| 2 | 0.62 to 0.78 | 195 | phrase work per thread fell from 0.35 s to 0.12 s |
| 3 | 0.65 to 0.9 | 135 | list postings fell from 22.2M to 12.1M |
| 4 | 0.54 to 0.68 | 134 | scan 0.27 to 0.34 |
| 5 | | 134 | phrase work per thread 0.125 s to 0.033 s |
| 6 | 0.35 to 0.45 | 134 | BOOL work per thread 0.08 s to 0.015 s |
| 7 | 0.44 vs 0.59 (same session) | 134 | scan: tokenizing and lookups overlap through prefetch |
| 8 | same as 7 | 92 | peak was the scan output plus the lists; now about one of them |
| 9 | same | 89 | query parse allocations gone |
| 10 | 0.33 vs 0.45 for the original (same session) | 92 | per-term scan state of common terms now compact |

## What the profile said, step by step

**Step 1 to 2.** Instrumenting the query phase showed phrase queries took 80% of it (about 4.2 s of CPU over 12 threads). Intersection took 14 ms per thread; the rest was verification, which re-tokenized and hashed 63M tokens, more than the whole corpus holds, because long documents are over-represented among candidates. Verification now walks the candidate's text and compares tokens to the rarest phrase term byte by byte, then checks the neighbours in place.

**Step 2 to 3.** Peak memory was two copies of the same 22M postings: the per-thread scan output and the final lists. Freeing the scan output in 1 MB blocks while scattering did nothing (195 to 201 MB), because the scatter touches every term's region, so the whole final array is resident almost at once. A histogram on the sample showed terms in more than 1/32 of documents hold about two thirds of the postings but number only about 350, so those became bitmaps (377 terms, 11 MB). At full scale 12.1M postings stayed lists. Thresholds of 1/64, 1/128 and 1/256 gave 124, 131 and 157 MB, so 1/32 stayed.

**Step 3 to 4.** Tokenizing alone took 0.09 s of the 0.39 s scan; the rest was the term lookup. The key was built with byte stores and read back as 64-bit words, which stalls store forwarding. It is now loaded straight from the text and lowercased with one OR (tokens are only `[A-Za-z0-9]`, and `| 0x20` lowercases letters and leaves digits). A table at load 1/2 instead of 1/4 measured the same or slightly faster over 9 alternating runs, with 2 MB less.

**Step 4 to 5.** The byte loop in the phrase check mispredicted at every token boundary. The anchor is rare, so a 32-byte AVX2 compare for its first letter (`byte | 0x20`) skips almost everything.

**Step 5 to 6.** BOOL was now the larger query cost: a bitmap term at the start of a query, or OR-ed in, expanded into a list of up to 250,000 ids, and each later step copied it again. The running result now stays a bitmap once that happens, so AND, OR and NOT against another bitmap are word loops, and count, sum and the ten smallest ids come from popcounts.

**Step 6 to 10 (second session).** A single-threaded breakdown of the scan showed tokenizing about 500 ms, term lookups about 290 ms and the per-term bookkeeping (dedupe, bitmap OR, list push) about 420 ms. Prefetching each token's table slot and resolving it 8 tokens later (step 7) gave the first large gain. Peak memory was the scan output (48 MB) plus the lists it is scattered into (48 MB): filing the output by term bucket and scattering bucket by bucket with kernel-mapped blocks (step 8) dropped the peak from 134 to 92 MB at the same speed; with malloc the freed blocks stayed resident, which is why an earlier free-as-you-drain attempt had saved nothing. Renumbering terms by sampled frequency (step 10) made the per-term state of the common terms compact and was about 8% faster.

## Tried and kept out

- Block-freed scan output on its own (step 2 to 3): no memory gain, explained above.
- Lower bitmap thresholds: more memory, no faster.
- Prefetching the term key as well as the slot (second prefetch stage): no change.
- Atomic versus plain OR for bitmap bits: about 5%, not worth the race at thread boundaries.
- Buckets balanced by sampled postings instead of 512 ids: hundreds more block lists, 96 MB and slower.
- Renumbering only the bitmap terms to the front: kept memory at 88 MB but lost the speed of the full frequency order.
