# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 56.35 | 2,200 | 0 |
| 1 | C++ rewrite: read queries first, index only their terms, 12-thread tokenize over mmap, galloping set ops, phrase check on candidates only | pending | | | |

## Notes

Step 1, measured locally before the first board run (laptop, Docker volume, 4 runs): 0.89 to 1.15 s wall and 195 MB cgroup peak on the full seed-1 set, output sha256 `f86973c2...` matching the published hash. The laptop has 16 threads and turbo, so the board number will differ.
