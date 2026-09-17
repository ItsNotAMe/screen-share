# GPU receive and bounded decoder recovery

The Windows room runtime prefers Media Foundation D3D11 H.264 decoding. Synthetic
and explicitly software decoder factories remain available. This changes the v2
UI/CLI receive path; ordinary legacy application routing is still gated.

## Ownership and presentation

- A decoder owns its MF transform on an MTA worker and a multithread-protected
  D3D11 device. Startup probes NV12 plane views before selecting GPU output.
- MF supplies DXGI samples. Copy the visible aperture into an independently owned
  NV12 texture before releasing the sample. This prevents MF surface-pool reuse
  from overwriting a queued frame. **One GPU copy remains; this is not strict
  zero-copy.** The ordinary path has no CPU readback, I420 conversion or re-upload.
- Publish at most eight GPU buffers per decoder configuration. A consumer holding
  all eight causes subsequent outputs to be dropped, without blocking decoding or
  invalidating codec references. Temporary output work is separately bounded to
  32 frames / 64 output iterations per call, with the existing allocation limits.
- The existing one-slot handoffs retain the GPU buffer/device. Qt and CLI pass the
  texture to the shared presenter, which adopts its device and samples its luma
  and chroma planes. SRVs retain the displayed texture for redraw. Existing
  one-frame DXGI queue, nonblocking presentation, minimize/hide handling and
  three-rebuild renderer recovery remain active.
- Offscreen rendering and explicit pixel assertions can request a cached CPU
  conversion. The frame's shared owner keeps that span valid. UI/CLI diagnostics
  distinguish retained GPU frames and requested GPU readbacks; these are local
  counters, not physical display timing or cross-machine latency.

The device-manager contract follows [Microsoft's MF D3D manager documentation](https://learn.microsoft.com/en-us/windows/win32/medfound/mft-message-set-d3d-manager).

## Recovery and dimensions

GPU initialization failure selects software immediately. Runtime GPU failure or
retirement stops the transform and quarantines hardware for that runtime's decoder
factory, including later Configure calls and replacement decoders. Explicit room
rejoin creates a new runtime/factory; ordinary codec reconfiguration does not retry
the failed hardware.
Recovery requires a new keyframe, waits at least 250 ms, and allows at most three
transform rebuild attempts until Configure/Release. Successful frames do not
refill that budget. Malformed input cannot trigger an unlimited rebuild loop.
MF/driver calls themselves are synchronous and cannot preempt a hung driver.

RTP/NTP association and the accepted dimension limit are stored together for
every submitted sample. In-flight frames from before a resize are checked against
their own dimensions. Previously a delayed 320×180 frame could be rejected against
the new 160×90 limit, unnecessarily forcing recovery. Both Windows frontends now
exercise that transition while requiring hardware decode to remain active.

GPU-to-software recovery preserves the selected dimensions. It does not change
manual resolution/FPS settings or bypass WebRTC bitrate control. Unknown/invalid
aperture metadata fails closed; fractional/odd/out-of-bounds crops are rejected.
1920×1088 coded output is cropped to its 1920×1080 visible aperture before display.
The internal decoded result retains coded dimensions; the published texture itself
contains only the visible image.

Receiver telemetry allowlists `Media Foundation H264 (D3D11 NV12)` as hardware and
the existing CPU label as software. The ordinary receiver reports the current
implementation; startup preference alone is not the reported state.

## Repeatable validation

Run `build/sdk-proof-release/MfDecoderAdapterTest.exe --gpu` (also Debug). It uses
synthetic encoded pixels and no window, audio endpoint or physical input. It checks
CPU decode, unavailable-GPU startup fallback, four GPU configurations including
1080p cropping, timestamps, callback ownership, retained pixels after release,
zero readbacks until explicitly requested, eight-buffer pressure, device retirement,
fixed-size software recovery, keyframe/backoff gating and exhausted corrupt-input
recovery. `SCREENSHARE_TEST_HARDWARE=ON` includes this in CTest with a 30-second
timeout as `mf-hardware-decoder-recovery`.

Run the desktop extension of `scripts/test-room-regression.py` for actual Qt/CLI
GPU handoff, hardware telemetry, live resize and renderer lifecycle. Its audio is
synthetic/discarded. Pixel assertions deliberately request readback; the separate
Qt renderer test switches native/CPU inputs and exercises recovery while asserting
zero readbacks and a one-frame presentation queue.

See HEADLESS-TESTING.md for exact artifacts and results. These checks establish
ownership, bounded recovery and integration, not faster measured gaming latency.
Real adapter/driver coverage, historical visible-but-occluded behavior, display-only
capture fallback/source identity/cursor/HDR acceptance, capture resource regressions,
network impairment and external image/input latency remain open. The broader video
group and Stage 2 are not marked complete by this receive-path implementation.
