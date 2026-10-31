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
| 11 | Scan output in 3-byte entries, posting lists as 16-bit per-thread offsets decoded per query | run-a75abbe64920 | 0.292 | 167 | 61.0 |
| 12 | malloc mmap threshold pinned, so freed scan state really goes back | run-5c032275f3b6 | 0.301 | 175 | 60.5 |
| 13 | Transparent huge pages off (the box backed partly written blocks with whole 2 MB pages) | run-e6924d8063c9 | 0.292 | 69.6 | 64.8 |
| 14 | Scan output as 16-bit units: term offset in its bucket and a 7-bit doc gap, with an escape for larger gaps | run-443f9c5c3311 | 0.290 | 59 | 65.56 |
| 15 | Scan output buckets of 2,048 terms (37 write streams per thread instead of 148) | run-060e2570f6d3 | 0.279 | 59.5 | 65.94 |
| 16 | Phrase anchors: the rarest term keeps its occurrences' file offsets, and a phrase is checked in place at each one | run-f9bbc8a6c0fa | 0.268 | 61.9 | 65.94 |
| 17 | Terms dealt round-robin over the scan buckets, per-thread posting regions, bitmaps from zero pages, term table sized up front, output in chunk buffers | pending | | | |

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
| 11 | 0.33, same as 10 (same session) | 76 | scan output 48 to 36 MB, lists 48 to 24 MB |
| 12 | 0.33, same as 11 | 71 | glibc had kept freed per-thread arrays resident (found on Track D) |
| 14 | 0.323 vs 0.339 (9 alternating runs) | 61 vs 71 | scan output 36 to about 24 MB |
| 15 | Scan output buckets of 2,048 terms instead of 512 (37 write streams per thread instead of 148), gap field 5 bits | run-060e2570f6d3 | 0.279 | 59 | 65.94 |

## What the profile said, step by step

**Step 1 to 2.** Instrumenting the query phase showed phrase queries took 80% of it (about 4.2 s of CPU over 12 threads). Intersection took 14 ms per thread; the rest was verification, which re-tokenized and hashed 63M tokens, more than the whole corpus holds, because long documents are over-represented among candidates. Verification now walks the candidate's text and compares tokens to the rarest phrase term byte by byte, then checks the neighbours in place.

**Step 2 to 3.** Peak memory was two copies of the same 22M postings: the per-thread scan output and the final lists. Freeing the scan output in 1 MB blocks while scattering did nothing (195 to 201 MB), because the scatter touches every term's region, so the whole final array is resident almost at once. A histogram on the sample showed terms in more than 1/32 of documents hold about two thirds of the postings but number only about 350, so those became bitmaps (377 terms, 11 MB). At full scale 12.1M postings stayed lists. Thresholds of 1/64, 1/128 and 1/256 gave 124, 131 and 157 MB, so 1/32 stayed.

**Step 3 to 4.** Tokenizing alone took 0.09 s of the 0.39 s scan; the rest was the term lookup. The key was built with byte stores and read back as 64-bit words, which stalls store forwarding. It is now loaded straight from the text and lowercased with one OR (tokens are only `[A-Za-z0-9]`, and `| 0x20` lowercases letters and leaves digits). A table at load 1/2 instead of 1/4 measured the same or slightly faster over 9 alternating runs, with 2 MB less.

**Step 4 to 5.** The byte loop in the phrase check mispredicted at every token boundary. The anchor is rare, so a 32-byte AVX2 compare for its first letter (`byte | 0x20`) skips almost everything.

**Step 5 to 6.** BOOL was now the larger query cost: a bitmap term at the start of a query, or OR-ed in, expanded into a list of up to 250,000 ids, and each later step copied it again. The running result now stays a bitmap once that happens, so AND, OR and NOT against another bitmap are word loops, and count, sum and the ten smallest ids come from popcounts.

**Step 6 to 10 (second session).** A single-threaded breakdown of the scan showed tokenizing about 500 ms, term lookups about 290 ms and the per-term bookkeeping (dedupe, bitmap OR, list push) about 420 ms. Prefetching each token's table slot and resolving it 8 tokens later (step 7) gave the first large gain. Peak memory was the scan output (48 MB) plus the lists it is scattered into (48 MB): filing the output by term bucket and scattering bucket by bucket with kernel-mapped blocks (step 8) dropped the peak from 134 to 92 MB at the same speed; with malloc the freed blocks stayed resident, which is why an earlier free-as-you-drain attempt had saved nothing. Renumbering terms by sampled frequency (step 10) made the per-term state of the common terms compact and was about 8% faster.

**Step 13 to 14.** Memory by phase (cgroup `memory.current` read between phases): 8 MB after the queries, 66 MB at the end of the scan, a peak of 70 MB during the scatter, then 52 MB. The scan output was the bulk of it: 12.1M (term, doc) pairs at 3 bytes, about 36 MB, held until the scatter frees it. A thread meets its documents in order, so within one bucket's stream the docs never go down, and a pair fits in 16 bits as the term's 9-bit offset in its bucket and the 7-bit gap since that bucket's previous doc. Gaps of 127 or more (rare: a bucket of 512 terms usually has a posting every few docs) are written as 127 followed by a unit holding the whole gap. The scatter decodes the stream as it drains it. Peak 71 to 61 MB locally and the run slightly faster (less to write and read), same hash on full, sample, tiny, the worked example and a seed-5 sample checked against the reference.

**Step 14 to 15.** `perf` put the corpus scan at 61% of all CPU and the queries at 23%. Phase timers on variants with parts switched off split the scan's 165 ms: tokenizing about 50, term lookups about 50, and the per-term work about 65. Measured as whole runs with `perf stat -r 8` (the laptop's wall time was too noisy that hour), skipping the bitmap bits saved about 30 ms and skipping the posting-list path about 100 ms (its pushes plus the scatter and queries it feeds). Each (term, doc) pair was pushed into one of about 148 bucket streams per thread, so the store buffer and cache juggled 148 write streams. Wider buckets mean fewer streams: 10, 11 and 12 bucket bits all ran about 10% faster than 9 (0.27 to 0.29 s against 0.33 to 0.37 s), same 60 MB and hash. 11 bits it is, which leaves 5 bits for the doc gap; larger gaps take the existing escape unit. Two other ideas measured and dropped: 32-byte table slots holding the first 16 key bytes beside the id, so a lookup touches one line (about 3% slower, 5% more instructions), and plain ORs instead of locked ones for bitmap words no other thread can touch (no change). Same hash on full, sample, tiny, the worked example and the seed-5 sample.

## Tried and kept out

- Block-freed scan output on its own (step 2 to 3): no memory gain, explained above.
- Lower bitmap thresholds: more memory, no faster.
- Prefetching the term key as well as the slot (second prefetch stage): no change.
- Atomic versus plain OR for bitmap bits: about 5%, not worth the race at thread boundaries.
- Buckets balanced by sampled postings instead of 512 ids: hundreds more block lists, 96 MB and slower.
- Numbering terms rarest first, so the heaviest bucket is scattered last: about 5% slower and no smaller.
- Renumbering only the bitmap terms to the front: kept memory at 88 MB but lost the speed of the full frequency order.
- A Rust rewrite (commit ac3ebfb, run-27d474207408): every phrase checked during the scan at its rarest term, postings only for BOOL terms, a parallel query parse, per-thread posting regions and list terms dealt round-robin over the scan buckets. On the laptop it was slightly faster than step 15 (282 ms against 290 ms) with 52 MB against 60 MB; on the box it ran 0.360 s against 0.279 s, with 54.5 MB and 998 lines, 63.1 points against 65.9. Reverted; the laptop's timings (WSL2, very costly first-touch page faults) did not rank the two the way the box does.

## Board runs

Runs measured on the scoring box after the team was registered. M is the box's cgroup peak in MiB; K and points as the board reports them.

| Commit | Run ID | T (s) | M (MiB) | K | Points | What it measured |
|---|---|---|---|---|---|---|
| 59e9bc3 | run-a75abbe64920 | 0.2924 | 167.0 | 16.63 | 61.04 | Step 11 (3-byte scan output, 16-bit lists): 76 MB locally, 167 MiB on the box |
| 8041c6b | run-5c032275f3b6 | 0.3011 | 175.2 | 16.23 | 60.51 | Step 12 (malloc threshold): 71 MB locally, still 175 MiB on the box |
| b4606d2 | run-e6924d8063c9 | 0.2918 | 69.6 | 19.82 | 64.85 | Step 13: transparent huge pages off; the box now matches the laptop |
| c451b56 | run-83b833e14e8a | 0.2895 | 70.2 | 19.86 | 64.9 | Same Track B code |
| 5a6463e | run-03115bd4e2c1 | 0.2887 | 70.2 | 19.89 | 64.93 | Same Track B code |

## Step 16: phrase anchors

A phase profile of step 15 (timestamps between phases, laptop, full data) put phrase verification at about 57 ms per thread in the query phase: about 210,000 candidate documents were rescanned for 19,000 phrases, close to rereading the corpus. Every phrase's rarest term in the sample is now its anchor when it is in under 1/200 of documents (on the full data all 18,006 anchors have df at most 1,071 and occur 835,000 times in all). Anchors are numbered after every other term, so the scan recognises one by its id alone, and it records the file offset of each occurrence (3.5 MB in all) instead of a document list. A phrase is checked only at its anchor's occurrences, walking the neighbouring tokens back and forward up to the document's TAB and LF; the document id comes from a binary search of the documents' text offsets. Phrase work per thread fell from about 57 ms to about 14 ms. A first version looked up each token's anchor number in a per-term array and made the scan slower; numbering the anchors last removed that lookup. On the box: 0.279 to 0.268 s, but 2.5 MB and 73 lines more, so the points stayed level (65.94).

## Step 17: memory back down, serial work out

Cgroup `memory.peak` read between phases showed where step 16's peak came from:

- The scatter of bucket 0. Ids were numbered by frequency, so bucket 0 held the hottest lists, and its postings touched its whole region of the lists before its scan output was freed: about 8 MB above the end of the scan. Terms are now dealt round-robin over the buckets by frequency rank, so every bucket carries a similar share.
- The answers: fixed 128-byte slots for 50,000 queries (6.4 MB) plus a copy (1.5 MB), alive at the very end. Each chunk of 64 queries now writes into its own buffer.
- The term table kept its parse-time capacity (`vector::assign` to a smaller size keeps the buffer); it is now reallocated when the terms are renumbered.

Serial work went too: the bitmaps come from zero pages (`mmap`) instead of a 12 MB memset at the barrier, the renumbering sorts packed 64-bit keys, the term table is sized from the query file so parsing never rehashes, and each thread's lists take their own region of the posting array, so each thread turns its own counts into cursors and the 9 ms serial cursor pass is gone. Laptop: peak 64 to 59 MB, same speed; same output on full, sample, tiny, the worked example and a seed-5 sample.