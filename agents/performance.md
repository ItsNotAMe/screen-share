# Performance Progress

This is the running optimization ledger. Saved run reports contain the raw tables:

- `performance/performance-summary.md`
- `performance/performance-samples.csv`

The compact comparison sheet is `agents/performance.csv`.

Use this file to compare meaningful test runs after each optimization step. Prefer the same machines,
same room mode, same resolution/FPS/bitrate/audio settings, and similar desktop content when comparing
before and after.

## Test Scenarios

| Scenario | Purpose | Suggested Run |
| --- | --- | --- |
| Local loopback | Fast sanity check for report generation and obvious queue regressions. | One Share and one Watch on the same machine, separate UDP ports. |
| Same LAN | Normal low-latency baseline without Internet/NAT noise. | One host and one watcher on the same LAN. |
| Cross NAT | Real Internet room behavior. | One host and one watcher on different LANs. |
| Multi-viewer | Fanout and per-viewer queue behavior. | One host and two watchers, preferably different networks when available. |

## Reading A Report

1. Open the saved report zip from the test run.
2. Read `performance/performance-summary.md` for the quick table.
3. Use `performance/performance-samples.csv` when comparing detailed timelines.
4. Copy the meaningful averages/max values into `agents/performance.csv`.

The CSV's `estimated_video_e2e_*` columns are report estimates, not clock-synchronized
glass-to-glass measurements. They add the sender estimated video path
(`capture + encode + UDP queue`) to the receiver estimated video path
(`decode + preview playout delay`).
Audio-only rows intentionally leave video-only timing columns blank and put the audio evidence in
the notes column.

## Comparison Rules

- Compare release builds when judging real speed.
- Keep resolution, FPS, bitrate, encoder choice, audio setting, and network route the same.
- Treat local loopback as a sanity check, not proof that Internet performance improved.
- A good optimization should reduce queue time, drops, or frame time without hurting FPS or stability.
