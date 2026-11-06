# Live pipeline

The modular room backend is the normal UI/CLI route. Capture publishes owned
frames, WebRTC handles media transport, and viewers present through the shared
native D3D surface. See [runtime ownership](../docs/architecture.md) and
[known limitations](../docs/known-limitations.md). Historical gate/checkpoint
notes no longer describe current routing. Do not remove production capture,
codec or input code merely because the legacy diagnostics also use it.

2026-11-06 NV12 software input: MfVideoEncoder consumes CPU NV12 and shared GPU
NV12 readback without the former NV12-to-I420-to-NV12 round trip. Readback uses
the first requested format; I420-only callers retain direct conversion and do
not eagerly retain a second buffer. Requesting both formats derives the second
from owned CPU pixels without another GPU readback. Device retirement rejects
new maps, including cached ones. CPU preview materialization uses NV12 too.
The real four-software-encoder test decodes 32 padded/changing NV12 inputs with
zero ToI420 calls (32 before); generated GPU input uses eight shared readbacks
and zero I420 conversions. NV12 packing/MF input copies remain; this is operation
count evidence, not a new total CPU/memory/latency scorecard.

2026-11-06 shared scaling: D3dVideoFrameBuffer retains at most four successful
scaled outputs keyed by all six canvas/image geometry values, under its existing
mutex. Shared capture resources let matching viewer tracks reuse immutable GPU
pixels and their lazy CPU readback. Tracks, adaptation, mapping/preset metadata
and encoding stay independent. Failed/busy scales are not cached; retirement or
scaler quarantine bypasses cached output. Eviction does not invalidate consumer
references. Do not add a cross-frame cache or overwrite published textures.
The explicit generated-texture StreamSettingsTests --gpu comparison reduced
four-viewer 1080p-to-720p scales/readbacks from 96/96 to 24/24 over 24 frames.
This is operation-count evidence, not a new full-session CPU/memory scorecard.

2026-09-25 multi-viewer report: a stalled connection was still processing capture
frames (including shared-device scaling). NativeRoomRuntime now gates per-viewer
delivery until negotiation, connection and initial settings are ready; disconnect,
failure, video pause and shutdown close the gate. The real-peer synthetic isolation
test fails with the old delivery behavior and passes with the gate. This is a
candidate fix for the reported FPS drop, not a reproduction of the user's WAN/GPU
slowdown. Reports retain up to 63 retired connection snapshots (no SDP/addresses),
candidate counts, negotiated state, OS version and source GPU-drop counters.
The host/viewer UI exposes terminal connection failures instead of waiting forever.

2026-09-26 diagnostics expansion: see docs/diagnostics.md for bounded startup,
failure and measurement histories, correlation IDs and redaction. Production
Windows peers get default STUN discovery when unconfigured. Decoder low-latency
ICodecAPI support is optional on older implementations; supported-but-failing
properties retain errors. Full ICE+DTLS state controls connection readiness.
ICE restart success also reconciles lifecycle when the aggregate transport stays
connected and emits no new connected callback. Windows 7 and the reported WAN
pair still need verification on those machines.
