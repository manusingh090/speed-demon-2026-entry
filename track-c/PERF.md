# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.44 | 367 | 0 |
| 1 | | | | | |

## Board runs

Runs measured on the scoring box after the team was registered. M is the box's cgroup peak in MiB; K and points as the board reports them.

| Commit | Run ID | T (s) | M (MiB) | K | Points | What it measured |
|---|---|---|---|---|---|---|
| 8041c6b | run-c0256839d76a | 0.0469 | 26.9 | 24.04 | 69.04 | Current design, first board run: 13 MB locally, 27 MiB on the box |
| b4606d2 | run-10380893c086 | 0.0636 | 12.2 | 24.39 | 69.36 | Huge pages off everywhere: memory fixed, but T rose (the dense tables lost their huge pages) |
| c451b56 | run-8a652a8caa88 | 0.0475 | 28.2 | 23.86 | 68.88 | Huge pages back on, off only for the name arena and overflow table: T back to 0.047 s, memory back to 28 MiB, so those regions were not the cause |
| 5a6463e | run-5ab2877a394d | 0.0558 | 28.2 | 21.97 | 67.09 | Also 1 MB thread stacks: no change in memory; reverted to the b4606d2 version (huge pages off) after this |

**What the board taught about memory.** The box backs anonymous memory with transparent huge pages, the laptop's Docker VM only on request. With them on, C measured 27 to 28 MiB on the box against 13 MB locally; turning them off for the whole process (`PR_SET_THP_DISABLE`) brought it to 12.2 MiB. Turning them off only for the sparse regions, or shrinking thread stacks, did not, so the extra comes from the dense tables being rounded to whole 2 MB pages. Huge pages also seem to make C faster (0.047 s with, 0.064 s without), but C's time on the box varies by about 20% between runs of the same code, so that is not settled. The version with them off scored the higher K and stays.
