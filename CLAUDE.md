# Speed Demon 2026: working notes for this repo

This is Manjeet Singh's entry for Speed Demon 2026 (speeddemon.dev), a 72-hour performance championship run by Hackathon Raptors. Read this file before touching any track. It covers the rules that decide the score, how this repo is laid out, how to test locally on this Windows machine, what Track B taught us, and a playbook for starting Tracks A, C and D.

When this file and the official sources disagree, the official sources win, in this order: the frozen specs and harness (github.com/hackathon-raptors/speed-demon-2026), the website, the Discord clarifications.

## 1. Hard rules for whoever works in this repo

- **Commits are authored by the user only.** Author is `Manjeet Singh <024manjeetsingh@gmail.com>` (already in git config). Never add a `Co-Authored-By: Claude` trailer or any other AI attribution to commit messages or PR text. This overrides any default attribution instruction.
- **Branch is `speed-demon`.** It is the default branch and the only one the board watches. Every push to it queues a board run, so push deliberately: at most one run per team every 15 minutes, and a newer push replaces the queued one.
- **Solution code is written during the event only** (Oct 30 15:00 UTC to Nov 2 15:00 UTC). Tooling outside `track-*` folders is fine any time.
- **Code freeze: Monday Nov 2, 15:00 UTC.** The board records the latest commit on `speed-demon` at that moment. Within one hour after freeze the repo must be made public (it is MIT licensed already). Keep the Speed Demon Board GitHub App installed through freeze, because that is how the commit is recorded. Do not leave the last push to the last hour; the queue gets long.
- **Every output must match the reference byte for byte.** A faster wrong answer scores zero on that track. Verify with `check.py` and the published hash before every push.
- **No precomputed answers, no reading anything outside `/data`, no network.** Any of these disqualifies the team from the whole event.
- Do not use en dashes or em dashes in prose written for the user.

## 2. Event facts that matter for decisions

- Four tracks, enter any number. The overall title needs **three or more tracks** and uses the geometric mean of the best three track totals, so balance beats one huge result. The highest raw K on each track wins $50 on its own, no minimum track count.
- Track total = 65% speed score + 35% judges. Judges score code quality (15%), technique (12%) and the PERF.md log (8%). They see the board run history, so every PERF.md step should cite a board run ID.
- Ties: raw K, then wall time, then earliest commit.

### The efficiency coefficient

```
K = (T_ref/T)^0.50 x (M_ref/M)^0.20 x (S_ref/S)^0.15 x (D_ref/D)^0.15
speed score = 50 x log10(K), clamped to [0, 100]   (K=10 is 50 points, K=100 is 100)
```

- **T** wall time, process exec to exit, mean of the middle three of five runs. No cap. This is the axis to spend time on: halving T multiplies K by 1.41.
- **M** cgroup v2 `memory.peak` of the container. Capped at 50x better than the reference. `/data` input pages do **not** count (the host preloads them into page cache), so `mmap` of the input is free. Anything written to `/tmp` (tmpfs) **does** count, as does every heap page. Halving M multiplies K by 1.15.
- **S** code lines counted by `scc` after the organisers' formatter (their settings, not ours), over the whole track folder, Dockerfile and build scripts included; README, PERF.md, manifests and lockfiles excluded. Capped at 1.0: code shorter than the reference earns nothing, longer costs. Doubling S multiplies K by 0.90. Do not compress code to save lines; readability is worth judge points.
- **D** = 1 + direct non-stdlib runtime dependencies in the manifest. Reference D = 1. One dependency multiplies K by 0.90. We use zero dependencies everywhere.

Quick rule of thumb: a change that adds 50 lines to a ~550-line file must buy about 2% speed to break even.

### Reference numbers (measured on the scoring box) and limits

| Track | Task | Reference T | Board cap (3x) | Final timeout (10x) | Output file |
|---|---|---|---|---|---|
| A Ingest | per hour+endpoint count, p50/p95/p99, 5xx | 32.05 s | 96 s | 321 s | `summary.txt` |
| B Index | 50k BOOL/PHRASE queries over 250k docs | 56.35 s | 169 s | 564 s | `results.txt` |
| C Join | hostile CSV parse, join, per-country aggregates | 14.44 s | 43 s | 144 s | `aggregates.txt` |
| D Traverse | 2,000 shortest paths on a 5M-edge graph | 72.69 s | 218 s | 727 s | `distances.txt` |

Reference memory is roughly: A 228 MB, B 2.2 GB, C about 400 MB, D about 900 MB (local M4 figures from the specs; the board reports the box values). The memory cap means going below reference/50 earns nothing more: for A that is about 4.6 MB, so on A memory gains run out fast.

### The scoring machine

Intel Core i5-13500, turbo **off** (fixed 2.5 GHz base), container pinned to the 6 P-cores = **12 hardware threads**. AVX2 yes, **AVX-512 no**. 8 GB memory, no swap, 512 processes, 1 GB tmpfs at `/tmp`, read-only root, no network at build or run time, `io_uring` blocked by seccomp, `mmap` fine. Design parallelism for exactly 12 threads. Expect the box to be roughly 1.5x slower than this laptop per thread.

### The sandbox contract for every track folder

- Read inputs from `/data` (read-only), write exactly one file to `/out`, exit 0. `TRACK` env var is set to the letter.
- The Dockerfile builds offline (`--network=none`) in at most 10 minutes, image at most 2 GB.
- Every `FROM` must be an allowlisted image pinned by digest (speeddemon.dev/allowlist.json). `COPY --from` only from earlier stages of the same Dockerfile. No `ADD <url>`, no `# syntax=`, no cache/secret/ssh mounts, no `ONBUILD`.
- `-march=native` is allowed; the board then builds that track on the scoring box itself, so native means the i5-13500.
- The allowlist is closed. Images we use:
  - build: `gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95`
  - runtime: `debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad`
  - others available: rust 1.99.1 (`rust:1-slim-trixie@sha256:1a939748a7cbab172de91a014e3804a97af1394a8c844faa67fce9401c4e40fb`), Go, GCC 15, Java 25, GraalVM, Node, .NET, Swift, Haskell, Python 3.13.

## 3. Repo layout

```
speed-demon-2026-entry/          github.com/manusingh090/speed-demon-2026-entry (private until freeze)
  CLAUDE.md                      this file (not scored)
  check.py                       organisers' local checker: builds a track offline and runs it in the board sandbox
  tools/                         our helpers (not scored, outside track folders)
    get-data.ps1                 download a track's full dataset and load it into Docker volume sd-<track>
    cmp.ps1                      benchmark one or more images on sd-<track>: median ms, peak MB, hash
    board.py                     summarise the live board, or one team's runs
  track-a/ ... track-d/          one build context per track; only these folders are scored
    Dockerfile  .speed-demon.toml  PERF.md  README.md  <source>
  data/                          gitignored: datasets (<track>-full, <track>-sample, <track>-tiny, b-worked)
  .out/                          gitignored: check.py output
../harness/                      clone of hackathon-raptors/speed-demon-2026 (specs, generators, references)
```

Each track folder is built and measured on its own. Code shared between tracks must be copied into each folder and counts toward S in each.

A track folder that is byte-identical to the starter (ignoring Markdown and `.speed-demon.toml`) is reported `unchanged` and never runs. All four tracks now have solutions.

`.speed-demon.toml` per track: set `team`, `language`, `dependency_count`. Track B has `team = "manusingh090"`, `language = "C++"`, `dependency_count = 0`. Make the team name match what was entered on the Tally form.

## 4. Status

| Track | State | Local wall | Local peak | Notes |
|---|---|---|---|---|
| A | C++, pushed, all hashes pass incl. worked example | 35 to 46 ms | 33 MB | S 222 after clang-format (was 373; ref 56); see track-a/README.md |
| B | C++, pushed, all hashes pass incl. worked | 0.30 to 0.35 s | 71 MB | S about 790 after clang-format (ref 91); tiny hash dcdad7ce..., worked 9c296c06...; see section 7 |
| C | C++, pushed, all hashes pass incl. worked and rules examples | 62 to 77 ms | 13 MB | S about 374 after clang-format (ref 108); see track-c/README.md |
| D | C++, pushed, all hashes pass incl. worked | 0.42 to 0.45 s | 85 MB | S about 285 after clang-format (ref 43); tiny hash ecaca77d..., worked 2e9871bb...; see track-d/README.md |

Board snapshot, Oct 30 17:35 UTC (58 teams; refresh with `python tools/board.py`). Board T values are the box's own timer and read much lower than our laptop's in-container timer, so compare K, not seconds:

| Track | Teams with runs | Best K | Leader's T / M / S |
|---|---|---|---|
| A | 38 | 37.8 | 0.03 s / 28 MB / 210 lines |
| B | 26 | 20.5 | 0.27 s / 69 MB / 832 lines |
| C | 29 | 26.0 | 0.07 s / 10 MB / 275 lines |
| D | 33 | 17.9 | 0.27 s / 86 MB / 508 lines |

For the overall title we need three tracks. D has the lowest bar and a clear algorithm; C rewards care more than raw speed; A is the most crowded and the bar is very high.

Board steps: repo submitted via the Tally form and the board app installed by the user (not by Claude; both need the user's own accounts). Check with `python tools/board.py --team manusingh090`.

## 5. Local workflow on this machine (Windows 11, Git Bash + PowerShell, Docker Desktop)

Toolchain reality: there is **no usable local compiler** (MinGW GCC 6.3 is too old and has no `mmap`; no Rust, no Go). Everything compiles inside Docker with the same image the board uses. Docker Desktop must be running (`Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"` if `docker version` cannot reach the engine). Python 3.11 is available for scripts.

### Get data

```powershell
powershell -File tools\get-data.ps1 -Track c        # full seed-1 data into data\c-full and volume sd-c
```

Smaller scales for fast iteration, from the harness generators (Python 3.11 works and reproduces the published hashes):

```bash
python ../harness/generators/c_join.py --seed 1 --scale sample --out data/c-sample
python ../harness/generators/c_join.py --seed 1 --scale tiny   --out data/c-tiny
```

### Expected output hashes (seed 1)

| Track | full | sample |
|---|---|---|
| A `summary.txt` | `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` | `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` |
| B `results.txt` | `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927` | `eeeec1e9319da98a6f3c664c78f93e7078fd603c25c2bb3027174e81e3638a8f` |
| C `aggregates.txt` | `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536` | `9035fed36823232dd41ae2d1b9d6d21aa7c73f49d8e95647e953a89d5ebe9a66` |
| D `distances.txt` | `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e` | `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0` |

These come from the specs; the hashes published on speeddemon.dev/board#datasets are authoritative if they ever differ. For `tiny` (or any data you generate), run the unchanged reference once and use its hash: `python ../harness/reference/c_join.py --data data/c-tiny --out <dir>` then `sha256sum <dir>/aggregates.txt`. Each spec also has a hand-written worked example (Track B's is in `data/b-worked`); build it as a dataset and check it too, because it hits the edge cases on purpose.

### Correctness check (the gate)

```bash
python check.py c --data data/c-sample --expected <sample sha256>
python check.py c --data data/c-tiny   --skip-build --expected <tiny sha256>
python check.py c --data data/c-full   --skip-build --expected <full sha256>
```

`check.py` also verifies every `FROM` is allowlisted and pinned. The time it prints includes container start-up and the slow Windows file share; ignore it and use `cmp.ps1` for timing.

### Timing and memory

```powershell
docker build --network=none -t c-new track-c
powershell -File tools\cmp.ps1 -Track c -Images c-new
powershell -File tools\cmp.ps1 -Track c -Images c-old,c-new -Runs 9    # A/B two versions
```

Laptop timings swing by 30% between runs. Never decide on one run: alternate versions with `cmp.ps1` and compare medians. When a change is under about 5%, measure it a second time.

### Profiling a phase

There is no `perf` inside Docker Desktop. What worked on Track B: copy the source to a scratch folder, insert `steady_clock` timestamps and `/proc/self/status` (VmRSS) plus `/sys/fs/cgroup/memory.current` reads between phases, print to stderr, build that copy as a separate image. Also time per query type and per thread, and count work done (candidates, tokens). Keep instrumentation out of the committed source. To test "what if this part were free", build a variant that skips it (for example tokenize without hash lookups); the difference is the cost.

### Windows gotchas we hit

- **Git Bash rewrites paths** in `docker run` arguments: `--tmpfs /out` becomes `C:/Program Files/Git/out`, and `-v C:\...:/data` breaks on the colon. Run `docker run` from PowerShell, or prefix the Bash command with `MSYS_NO_PATHCONV=1`. `gh api /user/...` has the same problem; drop the leading slash.
- **Python inside a Bash heredoc**: a `"\\n"` meant for a C string came out as a real newline. Write instrumentation scripts to a file instead of a heredoc.
- **Stale `.out\track-x`**: if `check.py` fails with `FileExistsError` on `.out`, delete it from PowerShell (`Remove-Item -Recurse -Force .out`) and rerun.
- **Bind mounts from OneDrive are slow** and charge page cache to the container, which inflates both time and memory readings. That is why `get-data.ps1` copies into a Docker volume.
- Git warns about LF/CRLF; `core.autocrlf=input` is set so the repo stores LF.

### Commit and push

```bash
git add track-c tools CLAUDE.md
git commit -m "Track C: <what changed and why>"      # no AI co-author trailer
git push origin speed-demon
```

Then after the run appears (`python tools/board.py --team manusingh090`), copy its run ID, T, M and points into that track's PERF.md table.

## 6. Playbook for a new track

1. **Read the whole spec** in `../harness/specs/<x>.md`: grammar, every edge case, output format, ordering, banned libraries. Then read `../harness/reference/<x>.py`; its output is the definition of correct.
2. **Get data**: tiny and sample from the generator, full via `get-data.ps1`. Record the reference hash for tiny.
3. **Write the straightforward fast version first** in C++20, no dependencies, one `main.cpp`, same Dockerfile shape as Track B (gcc:16 build with `-O3 -march=native -static -pthread`, copy the binary into debian:13-slim). Get every hash passing before optimising anything.
4. **Push once correct**, so there is a board baseline and a run ID for PERF.md.
5. **Profile, then fix the biggest phase.** Repeat. Log every step in PERF.md with the number before and after, including experiments that failed (judges reward those).
6. Fill README.md (approach, dependencies: none, next 24 hours).
7. Before freeze: final hash checks on all scales, then push with time to spare.

Skeleton Dockerfile (copy from `track-b/Dockerfile`, change the output binary name if you like):

```dockerfile
FROM gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95 AS build
WORKDIR /src
COPY main.cpp .
RUN g++ -std=c++20 -O3 -march=native -static -pthread -o /run main.cpp

FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
COPY --from=build /run /app/run
ENTRYPOINT ["/app/run"]
```

Delete the reference files (`run.py`, `<x>.py`) from the track folder when replacing them; they would count toward S.

### Track A: Ingest (3.1M log lines, 239 MB, ref 32 s)

- Done (see `track-a/README.md`). Worked example dataset is `data/a-worked` (reference hash `0782f9b9a885bdbcdfadf330f6bea55accdc06018c0108f69ea43372d2fac692`); tiny hash `c24fa507c963eaeac9224fd8435019b4f4e8c002ece7e32f316681a3957eb92f`.
- S is counted after `clang-format --style=LLVM` (80 columns), so long lines wrap and count twice. Check locally with the pip `clang-format` wheel. Reference S per track (harness/reference_axes.json): A 56, B 91, C 108, D 43.
- Laptop profile of the pushed version: parse about 20 ms (of which page faults 3 to 7 ms, SIMD scan alone 9 to 11 ms), hours 9 ms, write overlapped. MAP_POPULATE was slower (serial populate, +15 ms).

- Input `/data/requests.log`: `ts method endpoint status latency_us`, single spaces, LF. Timestamp always 27 bytes fixed width, span fixed to 72 hours (2026-10-30T18:00Z to 2026-11-02T17:59:59Z), so the hour index can be computed from fixed byte offsets (day and hour digits) with no date library. Lines are **not** sorted; no bound on disorder.
- Group by (hour, endpoint) where endpoint is an exact byte string; 1,842 endpoints, Zipf popularity, so a hash map from endpoint bytes to a small id, then a 72 x 1842 grid of groups.
- Percentiles are nearest rank: value at 1-indexed rank `(p*n + 99) / 100` of the sorted latencies, for p = 50, 95, 99. Use selection (`nth_element`) per group, not a full sort; or since n per group is small, sort small groups. Latencies are uint32 (0 to 4294967295 occurs).
- errors = status 500 to 599 (499 is not an error); error_ppm = `errors * 1000000 / count` floored, integer math.
- Output sorted by hour then endpoint as unsigned bytes (`memcmp`, shorter prefix first). Duplicate lines count twice.
- Parallel plan: split the mmapped file into 12 line-aligned ranges, each thread collects latencies per group into per-thread buffers, merge per group, then select percentiles in parallel across groups.
- Banned: dataframe/SQL engines, datamash/miller/xsv-type tools. Hash maps, sorting, SIMD are fine.
- Memory: reference 228 MB, cap at 50x means anything under ~4.6 MB is wasted effort; storing all 3.1M latencies as u32 is 12 MB, which is fine.
- Current board leader K is about 37, so the bar is high here. Some top entries finish in under 0.1 s.

### Track C: Join (3.32M CSV records, 204 MB, ref 14.4 s)

- Done (see `track-c/README.md`). Hand-written datasets: `data/c-worked` (reference hash `72ed9e59bff2dad354a9fb4cbc2d4b102477f634230b47e86035864c88d27444`) and `data/c-rules` (`0c084eef633858f79b023d1d767c8c474e73ad5c37f3f587c1145c3aa36e52ec`, LF-only header, last record without terminator, BOMs inside quotes), both built from `WORKED_*` and `RULES_*` in `../harness/tests/test_c_join.py`. Tiny hash `e4e43bc39920e9d446c3fcd700fdd35b3959e9495c6aaa738491df38a1aed733`.
- What was measured: name interning dominates. Shared bitsets written by every thread are fine as atomic ORs, but a shared cache line that every thread writes (name entry plus its country bits in one line) was much slower. Logging (country, name id) pairs per thread cost 13 MB more and was slower. Batched prefetch helped little; a rolling pipeline (probe, look up 16 records later, mark 16 after that) gained about 10% single-threaded. Huge pages raised M to 17 MB for no clear gain.
- `std::barrier` runs its completion function at every phase, not only the first: guard it.

- This is the correctness track. Section 2.1 of `c-join.md` is the parser contract; follow it exactly: quoted fields with `""` escapes, LF inside quotes is data, CRLF or LF terminators, a UTF-8 BOM (`EF BB BF`) at the start of the file **and** after some record terminators (skip it only there; inside quotes it is data).
- Join `region_id` against `/data/regions.csv` (plain, ascending ids up to 999,999, so a direct array `region_id -> country index` works; ids above the range or missing map to `_UNMATCHED`).
- Per country: count, id_sum, amount_sum/min/max in integer cents (no floats, never print `-0.00`), names_distinct (exact bytes after unescaping; empty quoted and unquoted are the same), name_bytes and note_bytes after unescaping. Sorted bytewise, `_UNMATCHED` last.
- Hard part for parallelism: splitting the file mid-way requires knowing whether a position is inside quotes. Options: a fast first pass that tracks quote parity (count `"` bytes; parity flips at each), or split at candidate boundaries and resolve by re-parsing from a known-safe point. Records never contain CR inside quotes, and notes never start or end with LF, which helps.
- names_distinct per country needs a set of byte strings per country (100,000-name pool), so a hash set of (country, name hash) with collision-safe comparison.
- CSV parser libraries are allowed but cost D; write our own.
- Reference is only 14.4 s, so wall time ratios are smaller here; the gate (correct output) is where most teams fail.

### Track D: Traverse (5M edges, 1.74M nodes, 2,000 queries, ref 73 s)

- Done (see `track-d/README.md` and `PERF.md`). What mattered: renumbering nodes for locality (random ids cost about 480 ns per settled node), and the shape of the order: compact 512-node blobs beat both a global BFS order and a few large BFS regions. Per-thread dense distance arrays are 167 MB; recycled 4 KB pages cost a few MB.

- `graph.txt`: header `n m`, then `u v w` lines, unsorted, weights uint32 including 0. Parallel edges, duplicates and self-loops exist. Answers can exceed 2^32 (use u64 distances), `-1` when unreachable, `0` when s == t.
- Most of the time is parsing 92 MB of integers and building a CSR adjacency (count out-degrees, prefix sum, scatter), all parallelisable.
- Queries are bounded by Dijkstra rank (median target is the 512th settled node, max 131,071), so plain Dijkstra with early exit when t is popped is the right algorithm; contraction hierarchies rarely pay off. Run the 2,000 queries on 12 threads, each with its own distance array reset lazily (a visited list or a generation counter instead of clearing 1.7M entries).
- A radix heap or 4-ary heap beats `std::priority_queue` here. Node ids are randomly relabelled, so locality is poor; renumbering nodes in BFS order after building the graph can help cache behaviour.
- Watch the unreachable queries: a target with no in-edges makes a one-directional search settle the whole main body. Check cheaply first (target in-degree 0, source out-degree 0, island membership) or use bidirectional search.
- Banned: any shortest-path or routing library, and anything that builds CH or hub labels for you. Plain containers and heaps are fine.

## 7. What Track B taught us (apply everywhere)

Final Track B design (see `track-b/README.md` and `track-b/PERF.md`): read queries first and index only their 75.7k terms; mmap the corpus; 12 threads scan line-aligned ranges; terms in more than 1/32 of docs (chosen from a 1/32 sample) become bitmaps, the rest become CSR posting lists via per-thread counts and a scatter; BOOL left to right with galloping and a bitmap running result; PHRASE by intersection then an AVX2 scan for the rarest term in each candidate.

Lessons, each one measured:

- **Profile before optimising.** The first guess (index build) was wrong; phrase verification was 80% of query time.
- **Count the work, not just the time.** Counting tokens processed showed we re-tokenized 63M tokens, more than the corpus has, because long documents dominate candidate sets.
- **Byte stores followed by a wide load stall the CPU** (store forwarding). Build keys with 8-byte loads plus a mask instead of byte by byte.
- **`x | 0x20` lowercases letters and leaves digits alone**, so ASCII alphanumeric tokens lowercase eight bytes at a time.
- **glibc keeps freed large blocks resident** once its dynamic mmap threshold has risen (after the first large free). `mallopt(M_MMAP_THRESHOLD, 1 << 16)` at the start of main makes freed arrays really go back: 17 MB on Track D, 5 MB on Track B, nothing on C.
- **Peak memory is about what is alive at the same time.** Freeing a buffer while scattering into another array does not help if the scatter touches every page of the destination early. Shrink what you store instead (bitmaps for dense sets).
- **mmap the input.** Input pages are not charged to us, so reading the 254 MB file costs 0 MB of M.
- **Dense sets as bitmaps, sparse as sorted lists**, and keep a running result as a bitmap once it gets big; count and sum come from popcounts (sum of bit positions = sum over k of 2^k x popcount(word & mask_k)).
- **A SIMD first-byte scan beats a byte loop** whenever you are looking for something rare.
- **Fixed-size output slots per query** let queries run in any order on any thread and be written with one call.
- **Use the sample to make decisions at run time** (which terms are dense) instead of hard-coding thresholds from the public data; the scoring data is a different seed.
- `std::barrier` (C++20) is a clean way to run "sample, decide, then scan" inside one thread pool.

Second pass on Track B (Oct 31): the scan is bound by memory accesses, not tokenizing (a 35% faster tokenizer changed almost nothing end to end). What moved it: prefetching term-table slots 8 tokens ahead, renumbering terms by sampled frequency, and for memory, bucketing the scan output and freeing it with munmap while scattering (malloc keeps freed blocks resident, so free-as-you-go only works with kernel-mapped blocks).

## 8. Useful links

- Site and rules: https://speeddemon.dev (plain version /start, setup /kickstart, board /board)
- Specs, generators, references, harness: https://github.com/hackathon-raptors/speed-demon-2026 (cloned at `../harness`)
- Board feed JSON: https://s3.amazonaws.com/raptors.dev/speeddemon.dev/2026/board/board.json
- Datasets: https://s3.amazonaws.com/raptors.dev/speeddemon.dev/2026/data/track-<x>/full-seed1/<file>
- Image allowlist: https://speeddemon.dev/allowlist.json
- Team form (resubmit to change tracks): https://tally.so/r/Ar7g9z
- Register members: https://tally.so/r/q4redG
- Board app: https://github.com/apps/speed-demon-board
- Discord for questions: https://discord.com/invite/xfYPDZYqeh
