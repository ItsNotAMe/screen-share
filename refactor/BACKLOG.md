# Deferred backend optimizations

## Multi-viewer memory

Deferred at the user's request on 2026-09-18. Matching legacy's private-memory
footprint is not required for cutover. The latest reference fixture (one host and
four local receivers) measures approximately 522 MiB hardware / 924 MiB software
private memory, versus legacy's 398 / 500 MiB. This is not total GPU allocation,
whole-system memory, or evidence of sustained growth.

Later work may reduce per-peer codec resources and evaluate bounded shared
encoding for viewers with identical settings. Preserve independent congestion
adaptation and source/frame ownership. Any optimization still needs a fair
latency/throughput comparison. Do not replace this with an arbitrary lower-memory
requirement that delays the frontend refactor.

Still required: no unbounded growth, no resource accumulation across shutdown,
and usable behavior on the actual desktop/laptop. Four-software-viewer CPU load
is a separate responsiveness concern, not waived by deferring memory tuning.
