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
