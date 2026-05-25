# Repo Map

This is a native Windows C++ screen-sharing prototype. Public code lives under `src/`; durable repo memory lives under `agents/`, with `.codex/README.md` as a local-tooling entrypoint.

## Source Layout

- `src/app/ScreenShareApp.*`: callable app runner for command-line parsing, top-level sender/receiver loops, adaptation policy, stats printing, logging.
- `src/app/AppSessionBackend.*`: pure C++ `IScreenShareSession` adapter around typed Share/Watch app runner entrypoints, using memory runtime control on a worker thread, translating key live telemetry into typed session events/status snapshots, and exposing typed display/audio-device discovery for the UI.
- `src/app/ScreenShareMain.cpp`: tiny executable entry point that calls the app runner.
- `src/core/`: shared typed session API, typed Share/Watch command builders, runtime-control interfaces, and native core-library entry points that are intended to be used by both the CLI and UI.
- `src/capture/`: Windows Graphics Capture and DXGI Desktop Duplication capture path, HDR/scRGB handling, GPU scaling/NV12 generation.
- `src/codec/`: H.264 file encoder, H.264 stream encoder/decoder, H.264 bitstream helpers, encoder probing.
- `src/transport/`: UDP sender/receiver, media packet format, fragmentation/reassembly, receiver feedback snapshots.
- `src/audio/`: WASAPI capture/playback and Opus encode/decode.
- `src/render/`: native Win32/D3D11 receiver preview window and GPU NV12 presentation.
- `src/video/`: CPU NV12 conversion used for diagnostics and fallback paths.
- `src/transport/LanDiscovery.*`: opt-in LAN receiver discovery helper used by CLI and Qt UI.
- `src/transport/StunClient.*`: standalone STUN Binding Request helper used by CLI NAT diagnostics.
- `src/transport/SignalingClient.*`: WinHTTP client for the optional HTTP room server diagnostics.
- `src/ui/`: optional Qt Widgets desktop control UI. Live sessions and normal display/audio-device discovery use the app session backend; helper diagnostics still use short-lived CLI commands where useful.
- `signaling-worker/`: optional Cloudflare Worker TypeScript project for room membership and UDP candidate exchange. It is signaling only; media remains direct native UDP.
- `scripts/install-dev-deps.ps1`: Windows bootstrap script for MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm dependencies.

## Build Shape

- Debug preset output: `build/debug/ScreenShare.exe`.
- Release preset output: `build/release/ScreenShare.exe`.
- `ScreenShareCore` is a static library target containing the reusable native engine modules.
- `ScreenShareAppRunner` is a static library target containing the app runner and app session backend adapter; `ScreenShare.exe` is a tiny main linked against it.
- If Qt 6 Widgets and Svg are available at configure time, builds also output `ScreenShareUi.exe`.
- Normal default builds create portable zip packages; see `agents/packaging.md`.
