# Live pipeline

The modular room backend is the normal UI/CLI route. Capture publishes owned
frames, WebRTC handles media transport, and viewers present through the shared
native D3D surface. See [runtime ownership](../docs/architecture.md) and
[known limitations](../docs/known-limitations.md). Historical gate/checkpoint
notes no longer describe current routing. Do not remove production capture,
codec or input code merely because the legacy diagnostics also use it.
