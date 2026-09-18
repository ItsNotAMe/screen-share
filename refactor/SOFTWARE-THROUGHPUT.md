# Software throughput and readback allocation — 2026-09-18

The software fallback's roughly 36 fresh FPS at the reference 1080p60 workload
was not a capture-speed limitation. Its encoder needed only about 4 ms per
frame, but inefficient output consumed the assigned budget and WebRTC reduced
delivery. This group improves encoding efficiency without disabling transport
rate control, and removes per-frame GPU staging allocation on CPU readback.

## Implementation

- Enable CABAC for the shared software H264 encoder, which already emits High
  profile. Both legacy and v2 receive this change. Hardware encoding retains its
  existing entropy-coding policy. Microsoft documents the software encoder's
  CABAC default as disabled in its [H264 encoder reference](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder).
- Ignore unchanged bitrate assignments. Preserve the transform across small
  variations in WebRTC's measured input cadence: changes smaller than 10% of
  nominal FPS (minimum three FPS) do not restart it. A real 60-to-30 transition
  still restarts and emits a keyframe. Capture and transport still enforce the
  requested user cap; this does not override that cap.
- Reuse one owner-thread staging texture per D3D device for CPU readback.
  A size/format change replaces it, with no cache of old sizes. Published I420
  images remain independent; reuse must not mutate a frame retained by a caller.
- Add bounded encoder-assignment/keyframe tracing to the optional pipeline
  diagnostic and a direct `MfCodecProbe --rates [generated.h264]` diagnostic.
  Neither tracing nor bitstream dumping is enabled in the normal application.

## Diagnosis and rejected approaches

`build/encoder-rate-baseline` records software delivery around 33–35 FPS and
approximately 5.66 Mbps assigned bitrate. Setting explicit CBR before or after
media types, setting a shorter codec buffer, and restarting on every bitrate
assignment did not solve delivery. Those experiments are not retained.

Keeping cadence stable and reusing staging alone also failed to fix software
throughput (`build/encoder-rate-stable`, approximately 22–25 FPS). Do not credit
cadence hysteresis alone for the final improvement. Enabling CABAC with those
changes restored approximately 54 fresh FPS in the stage diagnostic
(`build/encoder-rate-cabac`), with about 29 ms p95 local image age.

The direct rate probe uses generated, changing 8-by-8 grayscale noise at 1080p60.
Each 180-frame phase measures its last 120 frames, excluding 60 warmup frames.
Its reported bitrate uses codec timeline duration, not wall-clock delivery or
network throughput. At assigned 12/6/3/12 Mbps, the original output measured
15.25/28.38/35.48/35.51 Mbps; the final output measured
12.00/8.41/8.41/8.41 Mbps. The original bitstream repeatedly reached maximum QP.
This supports a compression-efficiency limitation, not a claim that the rate
API simply ignores assignments. CABAC does **not** establish a hard bitrate
ceiling for arbitrary noise. WebRTC's defensive rate control remains enabled.

## Acceptance boundaries

The final sixteen uninstrumented controls pass. V2 software delivers 52.9 fresh
FPS with one viewer and 48.2 with four, versus improved legacy's 51.5/49.6.
Corresponding local image-age p95 is 29.6/40.7 ms versus 46.0/48.8 ms. V2
hardware measures 27.8/30.8 ms versus legacy hardware's 43.0/48.2, with similar
fresh delivery and lower measured CPU. Four-viewer memory remains higher in
both v2 variants, and software CPU also exceeds legacy with four viewers.

Release and Debug builds pass thirty targeted CTest checks in total: eleven
codec/media checks and four native runtime/CLI/UI/evidence checks per build.
They include actual software CABAC PPS parsing, stable-cadence/real-change
keyframe behavior, no-output hardware recovery, immediate decoder output,
bounded readback allocation and retained-frame/resize/retirement behavior.

Fresh silent packet checks pass Release bandwidth collapse and 5% loss, plus
Debug collapse, without changing validators. Healthy viewers remain isolated
and all recover. First impaired synthetic input responses still reach 1,304 ms
in Release collapse and 1,170 ms in Debug, versus the preceding group's
797/1,196 ms observations. Release's first transient is worse in this run;
do not claim a congestion-latency improvement from recovery passing. Later
Release collapse responses are 57–88 ms and recovery responses 31–62 ms.
The initial backlog remains a required follow-up. These are internal synthetic
responses at 640x360/30, not physical gaming latency.

Raw validation: `build/sdk-{proof,app}-{release,debug}/software-memory-*.xml`
and `build/software-memory-network-{release,debug}`. The committed
[compact evidence](evidence/software-throughput-2026-09-18.json) preserves both
fair matrices, stage traces, rejected cadence-only results, source/binary/report
hashes, all thirty test names, the three packet checks and codec diagnostic logs.

Use the latest fair table in [COMPARISON.md](COMPARISON.md), with the same shared
software change applied to legacy, for the performance verdict. The stage
diagnostic is not a substitute for the uninstrumented, alternating-order matrix.
The workload uses equal configured ceilings, not equal measured wire bitrate.
CPU includes local host and receivers; image age ends at CPU consumption.

Readback allocation tests establish a bounded scratch texture and independent
frame ownership. They do not establish a whole-process memory plateau. Four
v2 peers retain independent encoders and GPU decoders, whereas legacy shares
an encoder and uses software decoding. Sustained resources, GPU presentation,
game load, network recovery and physical image/input latency remain separate
acceptance gates. Keep the modular capture path; do not declare all Stages 2–4
complete from these local measurements.
