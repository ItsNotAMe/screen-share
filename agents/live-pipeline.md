# Live pipeline

The modular room backend is the normal UI/CLI route. Capture publishes owned
frames, WebRTC handles media transport, and viewers present through the shared
native D3D surface. See [runtime ownership](../docs/architecture.md) and
[known limitations](../docs/known-limitations.md). Historical gate/checkpoint
notes no longer describe current routing. Do not remove production capture,
codec or input code merely because the legacy diagnostics also use it.

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
