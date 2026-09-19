# Matched legacy/v2 packet comparison

## Result — 2026-09-19

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

Eight original four-viewer collapse runs overlapped a build. They were replaced
as a complete group by eight isolated runs using the identical executable;
all original logs remain retained. The scorecard uses 24 unaffected original
runs plus those eight repeats, not the better run selected individually.
[Compact evidence](evidence/matched-network-2026-09-19.json) records selected
paths, hashes, phase results, unaffected viewers, CPU and memory. Raw batches:
`build/paired-network-adaptive-20260919` and
`build/paired-network-four-clean-20260919`. This closes the matched impairment
comparison requirement, not the unrelated hardware, physical or service gates.

## Method

The comparison uses the improved legacy codecs, waits and low-latency sender,
with **legacy feedback bitrate adaptation enabled**. V2 keeps its existing
WebRTC congestion controller. Both use a 1080p60 generated WGC scene, a fixed
resolution, a 12 Mbps ceiling, the same timer policy and CPU image consumers.
Audio is disabled for legacy and paced/discarded for v2; nothing is played.

The pinned upstream `SimulatedNetwork` model applies only to viewer zero's UDP
ingress. Collapse is 20→4→20 Mbps. Loss is 0→5→0%, with 25 ms mean delay and
10 ms deviation during impairment. Each phase lasts 12 seconds. The model uses
seed 12345, a 256-packet queue and the same bounded wrapper in both cases.
Hardware/software encoding, one/four viewers and forward/reverse run order are
included. Remaining viewers provide a check for cross-viewer effects.

Legacy's native UDP receiver requires a test-only loopback relay to traverse
the same wrapper. The relay forwards encrypted packets unchanged and forwards
feedback without impairment. It adds a loopback hop; baseline measurements
include that cost. V2 injects the packet factory on its existing network thread.
The application supplies no factory and retains ordinary transport behavior.
No operating-system traffic shaping, firewall rule, driver or network policy changes.

Every 10 ms the fixture samples the age of the most recently consumed valid
image, including when no new image arrives. This prevents a frozen stream from
looking fast merely because only its occasional fresh frames were measured.
These are local CPU-consumer ages, not physical display/input latency. Equal
configured bitrate limits do not establish equal actual bitrate or visual quality.
Each backend retains its existing decoder implementation and adaptive behavior.

Build the application with the existing verified SDK and
`-DSCREENSHARE_WEBRTC_TEST_SOURCE_DIR=C:/dev/projects/screen-share/build/webrtc/checkout/src`,
then build `BackendComparison`. Only that test target links the additional
upstream network-model sources; CMake checks their pinned revision and rejects
modified model sources. Run:

```powershell
python scripts/compare-impaired-backends.py build/sdk-app-release/BackendComparison.exe build/paired-network-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev
```

The default is 32 runs. Each has a 140-second process timeout and its own retained
log/JSON. The runner continues through individual failures, verifies actual
encoder selection and enabled legacy adaptation, records hashes and separates
measurement validity from backend acceptance. Normal-load validators reject
impaired results. Evidence-validator tests reject missing/incorrect phases,
bypassed links, missing held-image samples and disabled legacy adaptation.

The first smoke pair and interrupted `build/paired-network-full-20260919` batch
inherited legacy's fixed-rate normal-load setting. They are **superseded diagnostic
controls**, not the fair adaptive comparison. Their artifacts are preserved.
Corrected results are collected in `build/paired-network-adaptive-20260919`.
