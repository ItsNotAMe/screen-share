# Capture decision and retained decoder frame — 2026-09-18

Keep the shared `DesktopCapturer` implementation behind v2's modular capture
interface and retain immutable frame ownership. Both backends already use that
implementation. The controlled capture-only result does not justify replacing
v2's ownership wrapper with the legacy borrowed texture pool.

## Capture isolation

Release, generated WGC motion window, 1920×1080, 60 FPS target, five-second
warmup and fifteen-second measurement. Both consumers perform synchronous CPU
readback and inspect the same scene marker. No encoder, decoder, transport,
audio or physical input is involved. Legacy uses its borrowed conversion pool
and fixed-rate capture schedule; the owned path uses v2's immutable texture
configuration and polling cadence. A borrowed texture never escapes to an
asynchronous consumer in this diagnostic.

| Capture path | First image-age p95 ms | Reverse-order p95 ms | Fresh FPS |
|---|---:|---:|---:|
| Legacy configuration | 25.193 | 25.227 | 50.7–52.6 |
| Owned configuration | 24.604 | 24.558 | 51.5–52.5 |

The roughly 13–15 ms whole-pipeline gap previously reported cannot be assigned
to capture on this evidence. These are short generated-window tests, not HDR,
DXGI, game-load or two-machine display measurements. The sub-millisecond
difference is not a broad capture speedup claim.

## Located delay and shared fix

Optional codec decorators timestamp actual encoder/decoder input and callbacks.
The trace pairs timestamps only within the sender or receiver: WebRTC randomizes
the transported RTP epoch, so subtracting sender and receiver RTP-associated
times directly is invalid. The image marker independently measures total age.
Storage is bounded and the normal comparison runs without these decorators.

Before the fix, hardware decoder input-to-output p50 was approximately 17 ms,
and p95 was 34–38 ms. Capture-return-to-encoder-input p95 was about 0.02 ms,
encoding about 2.6–3.0 ms and decoded-frame-to-CPU-consumer about 1.2 ms.
Decoder timing includes retained-frame waiting, not just decode execution.
Percentiles of separate stages must not be added as an exact latency budget.

The decoder used `MFVideoFormat_H264_ES`, although both callers provide one
complete Annex-B picture after transport reassembly. Microsoft's
[subtype contract](https://learn.microsoft.com/en-us/windows/win32/medfound/video-subtype-guids)
distinguishes fragmented elementary streams from complete-picture `H264` input.
Selecting `MFVideoFormat_H264` removes waiting for the next picture boundary.

The decoder's `CODECAPI_AVLowLatencyMode` also used the wrong VARIANT type and
ignored errors. It now uses and verifies `VT_UI4`, the documented
[H264-decoder exception](https://learn.microsoft.com/en-us/windows/win32/medfound/codecapi-avlowlatencymode).
Correcting that type alone did **not** fix the retained-frame test; the subtype
change was necessary. Both fixes live in the shared decoder and benefit legacy
as well as v2. Neither change is an architectural advantage exclusive to v2.

The strengthened regression requires every submitted frame to return during
the same decode call and all twelve frames per cycle before release. Previously
CPU, GPU and startup-fallback tests each returned 44 of 48 frames across four
cycles; with the fix each returns 48. Timestamp wrap, retained GPU texture
ownership, crop, restart, resize and recovery checks remain in the same test.

Four repeated post-fix stage runs pass: hardware decoder p95 is 0.427–0.443 ms;
with software encoding and the same hardware-preferred decoder it is
0.545–0.597 ms. Total image-age p95 varies (hardware 41.8–56.8 ms; software
64.7–93.8 ms), so the fair uninstrumented comparison remains the performance
scorecard. Software fresh delivery remains only 36.8–40.4 FPS in these traces.

## Alternatives tested

Holding v2 ownership/codecs/transport fixed and substituting the legacy fixed
capture cadence produced 63.5–68.9 ms total p95 before the decoder fix, versus
70.5–77.4 ms for the native cadence. Disabling prerender smoothing produced
63.8–64.6 ms. Neither experiment removed the decoder's one-frame wait. They
remain diagnostics, not production policy changes: short-run phase variation,
preset switching and audio synchronization prevent treating them as free wins.
Gaming's validated 10 ms playout request remains unchanged.

## Reproduction and boundaries

```powershell
python scripts/compare-pipeline-stages.py build/sdk-app-release/BackendComparison.exe build/capture-stages-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev
python scripts/compare-pipeline-stages.py build/sdk-app-release/BackendComparison.exe build/decoder-stages-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev --case codecs
python scripts/compare-codec-controls.py build/sdk-app-release/BackendComparison.exe build/decoder-fair-NEW --origin https://screenshare-signaling-v2.bit-yeet.workers.dev
```

The default stage runner has twelve cases; `--case codecs` repeats only the
four relevant codec traces after a decoder-only change. Ordinary image/codec
validators reject diagnostic and capture-only reports. Raw pre-fix evidence:
`build/comparison-capture-stages-final`; post-fix traces:
`build/comparison-decoder-stages`. The earlier invalid cross-epoch trace attempt
in `build/comparison-capture-stages` is preserved and excluded.

The original native-handle, four-viewer memory, software throughput, physical
latency/audio/input and two-machine game-load gates remain separate. This step
settles the capture replacement question and removes a measured decoder delay;
it does not close all Stage 2–4 acceptance.

## Final fair comparison and regressions

The uninstrumented matrix validates 16/16 runs with the decoder fix on both
backends. Hardware image-age p95 (median of two run percentiles) is 44.1 ms
legacy versus 50.8 ms v2 for one viewer, and 47.7 versus 44.7 ms for four.
The full scorecard and variance are in COMPARISON.md. Retain the one-viewer
gap and software-delivery/memory limitations rather than claim every metric wins.

Release and Debug each pass ten codec/media tests and four application tests:
immediate CPU/GPU/fallback output, timestamp wrap, crop and resize, retained GPU
ownership, deadline/quarantine/recovery, hardware and software transport,
public-session preset changes, native runtime, CLI media and offscreen Qt UI.
The comparison validator includes eleven cases, including rejecting capture-only
and instrumented reports as ordinary comparison evidence.

Rebuilding the older `MfCodecProbe` exposed a missing `dwmapi` link dependency
from `DesktopCapturer::InputBounds`; its target now declares that dependency.
The benchmark executable was not rebuilt or changed during measurement. Final
regression checks and SDK verification ran after the fair matrix finished.

Release bandwidth collapse and 5% loss, plus Debug collapse, pass the existing
packet/recovery validator with healthy-viewer isolation and about 30 FPS after
recovery. Their initial impaired-phase internal input responses still include
797 ms (Release collapse) and 1,196 ms (Debug collapse). These are recovery
passes, not proof that transient backlog or physical Gaming latency is solved.
Raw results: `build/decoder-network-release` and `build/decoder-network-debug`.
The [compact evidence](evidence/capture-decoder-2026-09-18.json) preserves these
limitations along with performance results and regression hashes.
