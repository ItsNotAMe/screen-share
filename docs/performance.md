# Measured backend comparison

These are local measurements from September 2026, not universal performance or
Internet-latency guarantees. Hardware/software, one/four viewers and run order
were controlled. Retain the modular backend; outstanding qualification is listed
in [known limitations](known-limitations.md).

## Latest scorecard — congestion-window follow-up

All 16 uninstrumented 1080p60/12 Mbps controls pass again after reducing v2's
additional in-flight allowance to 50 ms with bitrate pushback. Forward/reverse repeats preserve the
earlier local image-age and hardware CPU improvements. Portable codec/timer fixes
remain in both backends; legacy has no WebRTC congestion controller to configure.
These runs check normal-load regressions, not matched impaired-network superiority.

| Viewers / encoder | Legacy age p95 / v2 (ms) | Legacy fresh FPS / v2 | Legacy CPU / v2 (one core = 100%) | Legacy private MiB / v2 |
| --- | --- | --- | --- | --- |
| 1 / software | 45.3 / 29.8 | 52.5 / 52.5 | 184 / 119 | 435 / 311 |
| 1 / hardware | 44.5 / 29.2 | 53.1 / 52.1 | 57 / 24 | 333 / 239 |
| 4 / software | 49.9 / 41.5 | 49.2 / 46.1 | 406 / 530 | 492 / 912 |
| 4 / hardware | 48.7 / 33.0 | 49.9 / 51.9 | 274 / 95 | 407 / 531 |

Numbers are means of two runs, same-process generated WGC capture to CPU image
consumption. Legacy still decodes in software; v2 prefers hardware decode.
Configured ceilings are equal, actual rate/quality are not: one-viewer software
luma PSNR is 26.7 dB legacy versus 23.1 dB v2. Four-viewer software CPU remains
higher and fresh FPS is about 6% lower than legacy in this matrix, though both pass
the unchanged workload gate. Do not call this a universal quality/resource win.
Relative memory optimization remains backlogged. The historical compact evidence
retains hashes, per-run metrics and limitations in Git at commit `376d59a`.


## Matched network impairment (2026-09-19)

**Retain v2.** All 32 selected runs validate, with forward/reverse order for each
configuration. V2 sustains motion under both impaired links while legacy mostly
freezes. These are medians of the two runs' affected-viewer held-image p95 and
fresh FPS during the 12-second impairment, with legacy / v2 shown in each cell:

| Link / viewers / encoder | Held-image p95 (ms) | Fresh FPS |
| --- | --- | --- |
| Collapse / 1 / hardware | 11,179.5 / 187.1 | 1.2 / 28.2 |
| Collapse / 1 / software | 11,207.9 / 260.8 | 1.1 / 19.5 |
| Collapse / 4 / hardware | 11,172.0 / 194.8 | 1.2 / 28.5 |
| Collapse / 4 / software | 11,205.0 / 226.1 | 1.1 / 24.6 |
| Loss / 1 / hardware | 6,211.5 / 157.5 | 0.5 / 51.2 |
| Loss / 1 / software | 6,207.7 / 185.5 | 0.5 / 49.0 |
| Loss / 4 / hardware | 7,642.6 / 166.0 | 0.2 / 51.0 |
| Loss / 4 / software | 5,110.0 / 192.7 | 0.3 / 47.4 |

The result is not an every-metric win: after collapse, software legacy has lower
recovery-phase held-image p95 (77.9 vs 121.6 ms for one viewer; 86.8 vs 108.7 ms
for four) and higher recovery FPS. V2's reduction in freezing during impairment
is the principal improvement. The configured link is not a claim of real WAN
coverage, and these transient numbers do not replace the separate 150 ms settled
age gate or external gaming-latency measurement. Four-viewer software v2 also
uses more CPU (522–544% of one core versus legacy 360–365%) and private memory
(904–931 MiB versus 495–501 MiB). The hardware v2 configurations use less CPU;
healthy viewers remain responsive in both paths. These resource differences are
included in the evidence rather than hidden by the improved freeze metric.


Historical reports and raw evidence indexes are retained in Git at commit `376d59a`.
Reproduction tools remain in `scripts/compare-backends.py` and
`scripts/compare-impaired-backends.py`.
