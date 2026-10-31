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
| 17 | Terms dealt round-robin over the scan buckets, per-thread posting regions, bitmaps from zero pages, term table sized up front, output in chunk buffers | run-ddc4729fadbb | 0.277 | 59 | 65.78 |
| 18 | Query file parsed on 12 threads into one lock-free term table; table from zero pages and renumbered in place; counting sort at the barrier | run-c2b149a34d40 (with 23) | | | |
| 19 | Each term's postings in one stretch (12 segment lengths per term), laid out by all threads; doc lookup by page | run-c2b149a34d40 (with 23) | | | |
| 20 | Bitmap terms numbered first; each thread collects a term's bits per 64 documents and ORs them in once | run-c2b149a34d40 (with 23) | | | |
| 21 | Phrases checked by the scan at their anchor's occurrences, while the text is in cache; anchors keep doc lists for BOOL | run-c2b149a34d40 (with 23) | | | |
| 22 | Per-thread last-document marks and counts as 16-bit offsets; write cursors rebuilt per bucket, so the counts go before the scatter | run-c2b149a34d40 (with 23) | | | |
| 23 | Scan threads go on to lay out and scatter (one thread pool instead of two); `_exit` once the output is closed | run-c2b149a34d40 | 0.232 | 52.7 | 67.95 |
| 24 | Rewritten from scratch in Rust, short: about 600 rustfmt lines against 968 | run-880032be5ecf | 0.253 | 65.6 | 67.55 |
| 25 | Back to the C++ of step 23 while the Rust version is worked on | run-880032be5ecf (step 24) | | | |
| 26 | Rust again: queries stored flat, the sample's counts freed at once, rustfmt-friendly helpers; 557 lines, 55 MB | run-5231ce22564c | 0.241 | 58.0 | 68.86 |
| 27 | Per-token handler inlined into the token loop (it was one function call per token) and branch-free; terms numbered by class so a token's role is its id range; each term's postings in one stretch, terms dealt round-robin over 32 scan buckets; bitmaps borrowed, not copied, at query time; branch-free list merge | run-b6f929f84526 | 0.237 | 69.1 | 68.03 |

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
| 17 to 19 | 0.232 vs 0.271 (9 alternating runs) | 61 vs 59 | parse 26 to 12 ms, queries 47 to 34 ms per thread |
| 20 | 3 to 5% fewer cycles than 19 (`perf stat`, 3 x 12 runs each) | 61 | no locked OR per (term, doc) |
| 21 | 0.230 vs 0.302 median, 0.197 vs 0.238 best (11 alternating runs against step 17) | 56 vs 59 | phrase work per thread about 15 ms to almost none |

## Second session (Oct 31): serial work, query layout, phrases in the scan

The laptop is an AMD Ryzen 7 7840HS under WSL2 and Docker's `--cpus=12` is a CPU quota over 16 logical CPUs, while the box pins 12 threads to 6 Intel P-cores at a fixed 2.5 GHz, so wall time here is noisy (a 30% spread between runs) and serial code runs relatively faster here than there. Decisions in this session used `perf stat -r 12` cycle counts, three alternating rounds per variant, as well as wall time. Every step gave the published hash on full, the same output as step 17 on sample, tiny, the worked example and a seed-5 sample, and the reference's output on a hand-made edge set (tokens of 15 to 70 bytes around the 16 and 32 byte key limits, upper and title case, UTF-8 inside words, repeated runs, empty and token-free documents).

**Where the time went at step 17** (phase timestamps, laptop): parsing the queries 26 to 40 ms on one thread, the sample 7 to 11, the barrier 10 to 15, the scan 140 to 180, the scatter 16 to 24 and the queries 44 to 55 ms per thread. `perf stat` showed 8.1 of 12 CPUs busy on average, so about a third of the wall time was serial or waiting.

**Step 18.** The query file is split on line boundaries and parsed by 12 threads into one term table. An empty slot is claimed with a compare-and-swap that marks it busy until its key and id are stored; a thread that finds a busy slot with its own tag waits for the key before comparing, so every term still gets exactly one id. The table is sized from the exact number of term occurrences (spaces not followed by an operator) instead of every space and LF, comes from `calloc` (zero pages, no 4 MB memset), and is renumbered in place at the barrier (each slot's id rewritten) instead of rehashed. The barrier's sort of 75,000 terms is now a counting sort by sampled df. Parse 26 to 12 ms; the rest is page faults on the fresh table and the serial start of 12 threads.

**Step 19.** Cycle counts inside the query phase put decoding list postings at about 20 cycles per posting: a term's postings sat in 12 per-thread regions, so one decode touched 12 segment bounds and 12 separate stretches of the posting array. A term's segments now lie back to back in one stretch, with the 12 segment lengths as 16-bit numbers next to each other. All threads lay the lists out together after the scan (each sums its slice of the terms, then sets the starts and every thread's write cursors for that slice), then each scatters its own scan output. Decoding fell to about 10 cycles per posting. The same counts showed BOOL queries on an anchor term spending about 500 cycles per document in the binary search from a file offset to its document; a per-page table of first documents made that a short walk (step 21 later removed it).

**Step 20.** Bitmap terms are numbered first, so the scan tells them apart by comparing the id, and each thread keeps a pending 64-bit word per bitmap term for the current 64 documents. Repeats within a document cost nothing and the shared bitmap gets one atomic OR per (term, 64 documents) instead of one per (term, document).

**Step 21.** Anchored phrases cost about 15 ms per thread at query time: about 70,000 anchor occurrences per thread, each a random read into the 254 MB corpus. They are now checked by the scan at every occurrence of their anchor, while the text is in L1, and each match is kept as a (query, document) pair; the query phase only reads them back. The anchors become ordinary list terms in buckets of their own, for BOOL queries that use them, so the occurrence offsets (3.3 MB), their per-thread buffers (6.7 MB at the end of the scan) and the offset-to-document lookup are gone. Peak 61 to 56 MB; queries about 35 to 24 ms per thread.

**Step 22.** The per-thread scan state was 32-bit: the last document each term was seen in and its document count, about 400 KB each per thread. Both fit in 16 bits (a thread covers fewer than 32,768 documents). The counts were also kept through the scatter as write cursors; the cursors are now rebuilt for each bucket from the term starts and segment lengths in an 8 KB array, so the counts are freed before the scatter. Peak 56 to 53 MB, same speed.

**Step 23.** Two pieces of fixed cost. Freeing every structure on the way out (dozens of `munmap` calls) came just before the kernel tears the whole process down anyway; `_exit` after `fclose` saved about 4% of cycles in three paired `perf stat` rounds. And the scan threads no longer end and make way for a second pool of 12: the serial step between scan and scatter (gathering the phrase matches, sizing the list arrays) is the completion step of a barrier, and the same threads go on to lay out and scatter. Median 0.253 to 0.235 s over 9 alternating runs (step 22 against 23), same 53 MB.

**Measured and dropped in this session:**
- Each thread scattering the buckets in its own rotated order, so no two threads write the same term's stretch at once (testing for false sharing between neighbouring segments): the scatter took the same 16 to 20 ms, and memory rose to 67 MB because every bucket's stretch was touched early.
- Keys built with one 16-byte SIMD load and a length mask, or with `bzhi`, and a two-word lookup for tokens up to 16 bytes: 4 to 9% more instructions and the same cycles. The four-word build compiles well already.
- Forcing the per-token handler inline (GCC left it out of line): more instructions, no fewer cycles.
- One 8-byte entry per term for the last document and the count, instead of two arrays: no change.
- A second prefetch stage (read the slot four tokens after prefetching it, then prefetch the key and the term's scan state): 7% more cycles.
- The term table and keys on 2 MB pages (`MADV_HUGEPAGE`): no change, so the lookups are not TLB bound.
- Binary search of a list term's segments instead of decoding it, for AND and NOT against a small running result: 160M fewer instructions, the same cycles (the searches miss cache where the decode streams).
- Prefetching only the key of each token's home slot, four tokens ahead: more cycles.
- A 1/64 sample instead of 1/32: no clear change.
- Terms that occur only in phrases kept no lists: only 5.5% of list postings at full scale (625,000 of 11.3M), not worth the code.

**Step 24: a short Rust version.** On this track the code is 968 counted lines against a reference of 91, so S alone costs the C++ about a third of its K; a version at 60% of the size and the same speed and memory would score about 7% more. Written from scratch with the lessons of the C++ but none of its machinery: every phrase is anchored at its rarest sampled term (so the separate path for phrases without an anchor is gone), only BOOL terms are indexed, and the posting lists are built by one counting sort per thread. The first working version (correct on every file on its first build) ran 0.50 s at 91 MB; then, each change measured with `perf stat`: list OR as a merge instead of a sort (answers 230 to 140 ms), the malloc mmap threshold raised again before answering so large lists stop being fresh mappings, pre-sized list decoding and `write!` instead of `format!` (answers 98 to 25 ms per thread), tagged slots and terms renumbered by sampled frequency (9.9G to 8.7G cycles), 16-bit gap-coded scan output in 64 KB chunks and per-64-document pending bitmap words (91 to 72 MB), the 64-byte AVX2 tokenizer (8.8G to 7.5G cycles, 0.30 to 0.27 s), and one shared per-term table plus 16-bit per-thread counts (72 to 61 MB). Against step 23 on the laptop: 4 to 7% more cycles, 61 MB against 53 MB, 612 counted lines against 968, an estimated K about 2% higher. On the box (run-880032be5ecf) it ran 0.253 s against 0.230 s and 65.6 MiB against 53 MiB: K 22.44 against 22.93, the 7% the shorter code earns outweighed by the time (about -4.7%) and memory (about -4.2%). The C++ of step 23 is back (step 25) while the Rust version's scan and peak memory are worked on.

**Step 26.** The first Rust version lost on memory as much as on time, so the peak went first: the queries became three flat arrays with a span per query instead of two vectors each (100,000 allocations fewer), and each thread's sampled counts (3.6 MB in all) are freed as soon as they are summed instead of living to the end; 61 to 55 MB. Then the helpers were rewritten so rustfmt keeps them compact (it puts each call of a chain over 60 characters, and each argument of a long call, on its own line): 604 to 557 lines. Laptop, three `perf stat -r 12` rounds against step 23: 2 to 5% more cycles, wall within 3%, 55 MB against 53 MB, 565 counted lines against 968, an estimated K about 6% higher. On the box (run-5231ce22564c): 0.241 s, 58.0 MiB and 565 lines, K 23.83 against the C++'s best of 22.93, so the Rust version stays.

## Third session (Oct 31): what the token loop really costs

A different laptop this time: Intel Core Ultra 9 185H (6 P-cores with hyperthreads, 8 E-cores, 2 LP E-cores) under WSL2. Its kernel exposes no hardware counters, so there is no `perf stat`; every decision below comes from phase timestamps, per-thread CPU time and `cmp.ps1` medians over 9 to 15 alternating runs (spread about 5%). On battery the machine ran 30 to 60% slower and noisier, so only runs on mains power are compared. Step 26 measures 177 to 183 ms median and 53 MB here.

**Where the time went at step 26** (phase timestamps, best runs): query parse 8 to 12 ms, sample 6, barrier 5, scan about 100 to 130 (the hybrid cores finish their shares up to 2x apart), scatter 3 to 4, answers 33 to 45, output 1.3.

**Answers.** A per-query cycle count by kind put 60% of answer time on BOOL queries over list terms only. Each list term was decoded from 12 per-thread posting lists, each with its own start array: about 36 cache misses per term before any merging. Copying a bitmap term (31 KB) for every query that used one turned out to cost little; it is now borrowed (`Cow`) and copied only when an operator writes to it. Each term's postings now lie in one stretch, thread by thread, with 12 segment lengths beside it, so decoding reads one start, one 24-byte line of lengths and one contiguous run. Laid out in term order, though, the peak rose from 53 to 60 MB: during the scatter the first thread through the largest bucket touched every page of that bucket's whole region. The fix was to deal terms over 32 buckets round-robin by frequency rank (term i goes to bucket i % 32, 12 bits for its index and 4 for the gap) and lay the stretches out bucket by bucket, so each region is small: back to 53 MB, answers 25 to 20 ms. A branch-free AND/NOT merge took another 1.5 ms.

**The token loop.** Timing-only variants of the scan (wrong output, same work otherwise) split its 129 ms: no per-token work at all left 61 ms (tokenizing and lookups), no bucket writes 97, no phrase checks 119, no bitmap bits 117; with the per-thread state forced into cache 120, and the shared per-term info too, 102. The bucket write alone looked far too expensive, and stayed so with all writes sent to one cached slot. The emitted assembly explained it: the per-token closure was not inlined. Every token made a real call into a 397-instruction function that reloaded about ten captured pointers, because the phrase check (`around`) was inlined into the closure and the closure is reached from two places in the token loop. With `around` kept out of line (`#[inline(never)]`, it runs only at anchor tokens) and `#[inline(always)]` on the two closures that make the per-token path, the handler sits inside the token loop. Then the per-token work was made branch-free: a token that is no term gets the id one past the last term instead of a branch; every token writes its unit to its bucket and moves past it only if it is a new list posting; bitmap bits gather in one word per bitmap term for the current 64 documents and are ORed in when the block ends. Branch-free code without inlining measured the same as the branchy original (181 against 183 ms); inlined, branch-free beat inlined branchy by 7 ms (166 against 173 to 174).

**Roles from ids.** Terms are now numbered by class, then by sampled frequency within it: bitmaps, lists, lists that anchor a phrase, anchors only, and terms only phrases use. The scan reads a token's role off its id range, so the shared per-term info array is gone, and tokens of phrase-only terms share the "no term" id, so they need no per-thread state at all: 163 to 159 ms, 1 MB less.

**Measured and dropped:**
- `last` and `count` merged into one 32-bit array per thread (one line per rare term instead of two): 170 against 170 ms over 15 runs.
- Corpus pages mapped by the 11 idle threads (`MADV_POPULATE_READ`) while the main thread parses the queries: populating took longer than the parse and delayed the sample, 162 against 153 ms.
- Branchy bucket writes on top of the inlined branch-free version: 173 against 166 ms.

**Step 27 against step 26** (15 alternating runs): 162 against 180 ms median, 157 against 172 ms best, 51 against 53 MB. rustfmt counts 601 code lines against 557: the `#[inline(always)]` closure arguments make rustfmt put every argument of those two calls on its own line. Trims that do not change what the program does won some of that back (636 to 601): class bounds and anchor starts by `partition_point` instead of counting loops, the scan buckets as plain addresses so they derive `Default` and `Send`, and the posting array as relaxed atomics instead of a raw-pointer wrapper. Estimated K: time 1.054, memory 1.008, code 0.989, about 5% higher. Same hash on full, sample, tiny, the worked example, a seed-5 sample and a seed-21 tiny set through `check.py`, and on 25 repeated full runs.

**Step 27 on the board.** run-b6f929f84526 (commit 8d77fc0, the same code as 4ef6f6b): 0.237 s against 0.241 s, but 69.1 MiB against 58.0 MiB and 609 counted lines against 565, so K fell from 23.83 to 22.94 and the points from 68.86 to 68.03. The laptop's 10% did not show on the box, where the run is 1.6% faster. The memory moved the other way from the laptop's 53 to 51 MB. Neither Rust version turns transparent huge pages off, as the C++ did at step 13 (175 to 69.6 MiB on the box), so the likely cause is the new layout's regions being backed by whole 2 MB pages on the box: every thread now writes every bucket's region of one shared posting array at once, where step 26 wrote per-thread arrays. That is not measured yet.

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
| 3086948 | run-c2b149a34d40 | 0.2318 | 52.7 | 22.85 | 67.95 | Steps 18 to 23 together (Track B code of 6b4b312): T 0.277 to 0.232 s, M 59 to 52.7 MiB, the box agreeing with the laptop |
| ccc3bc5 | run-880032be5ecf | 0.2526 | 65.6 | 22.44 | 67.55 | Step 24, the first short Rust version: slower and larger than the C++ on the box |
| 0a8b262 | run-5231ce22564c | 0.2410 | 58.0 | 23.83 | 68.86 | Step 26, Rust with flat queries and 557 lines: the best Track B run so far |
| 8d77fc0 | run-b6f929f84526 | 0.2372 | 69.1 | 22.94 | 68.03 | Step 27: 10% faster and 2 MB smaller on the laptop, but on the box only 1.6% faster and 11 MiB larger, so K fell |

## Step 16: phrase anchors

A phase profile of step 15 (timestamps between phases, laptop, full data) put phrase verification at about 57 ms per thread in the query phase: about 210,000 candidate documents were rescanned for 19,000 phrases, close to rereading the corpus. Every phrase's rarest term in the sample is now its anchor when it is in under 1/200 of documents (on the full data all 18,006 anchors have df at most 1,071 and occur 835,000 times in all). Anchors are numbered after every other term, so the scan recognises one by its id alone, and it records the file offset of each occurrence (3.5 MB in all) instead of a document list. A phrase is checked only at its anchor's occurrences, walking the neighbouring tokens back and forward up to the document's TAB and LF; the document id comes from a binary search of the documents' text offsets. Phrase work per thread fell from about 57 ms to about 14 ms. A first version looked up each token's anchor number in a per-term array and made the scan slower; numbering the anchors last removed that lookup. On the box: 0.279 to 0.268 s, but 2.5 MB and 73 lines more, so the points stayed level (65.94).

## Step 17: memory back down, serial work out

Cgroup `memory.peak` read between phases showed where step 16's peak came from:

- The scatter of bucket 0. Ids were numbered by frequency, so bucket 0 held the hottest lists, and its postings touched its whole region of the lists before its scan output was freed: about 8 MB above the end of the scan. Terms are now dealt round-robin over the buckets by frequency rank, so every bucket carries a similar share.
- The answers: fixed 128-byte slots for 50,000 queries (6.4 MB) plus a copy (1.5 MB), alive at the very end. Each chunk of 64 queries now writes into its own buffer.
- The term table kept its parse-time capacity (`vector::assign` to a smaller size keeps the buffer); it is now reallocated when the terms are renumbered.

Serial work went too: the bitmaps come from zero pages (`mmap`) instead of a 12 MB memset at the barrier, the renumbering sorts packed 64-bit keys, the term table is sized from the query file so parsing never rehashes, and each thread's lists take their own region of the posting array, so each thread turns its own counts into cursors and the 9 ms serial cursor pass is gone. Laptop: peak 64 to 59 MB, same speed; same output on full, sample, tiny, the worked example and a seed-5 sample.