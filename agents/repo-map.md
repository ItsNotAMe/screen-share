# Repo Map

This is a native Windows C++ screen-sharing prototype. Public code lives under `src/`; durable repo memory lives under `agents/`, with `.codex/README.md` as a local-tooling entrypoint.

## Source Layout

- `src/api/ScreenShareAPI.h` and `src/api/ScreenShareAPI.cpp`: concrete `screenshare::ScreenShareSession` API facade for Share/Watch start, stop, runtime settings, status snapshots, events, display discovery, and audio-device discovery.
- `src/cli/ScreenShareCLI.*`: CLI runner for command-line parsing, CLI typed Share/Watch option construction, top-level sender/receiver loops, adaptation policy, stats printing, and logging.
- `src/runtime/ScreenShareRunContext.h`, `src/runtime/ScreenShareSessionRunner.h`, and `src/runtime/ScreenShareSessionRunner.cpp`: typed runtime-backed Share/Watch session runner plumbing used by the concrete API without including the CLI header.
- `src/runtime/ScreenShareRuntimeInternal.h`: private declarations shared only inside the session runtime implementation; not a public UI/backend API surface.
- `src/core/ScreenShareSession.*`: shared session data types and helpers used by the API, CLI, and UI.
- `src/cli/ScreenShareMain.cpp`: tiny executable entry point that calls the CLI runner.
- `src/core/`: shared session data types, legacy typed Share/Watch command-preview builders, runtime-control interfaces, and native core-library entry points that are intended to be used by both the CLI and UI.
- `src/capture/`: Windows Graphics Capture and DXGI Desktop Duplication capture path, HDR/scRGB handling, GPU scaling/NV12 generation.
- `src/codec/`: H.264 file encoder, H.264 stream encoder/decoder, H.264 bitstream helpers, encoder probing.
- `src/transport/`: UDP sender/receiver, media packet format, fragmentation/reassembly, receiver feedback snapshots.
- `src/audio/`: WASAPI capture/playback and Opus encode/decode.
- `src/render/`: native Win32/D3D11 receiver preview window and GPU NV12 presentation.
- `src/video/`: CPU NV12 conversion used for diagnostics and fallback paths.
- `src/transport/LanDiscovery.*`: opt-in LAN receiver discovery helper used by CLI and Qt UI.
- `src/transport/StunClient.*`: standalone STUN Binding Request helper used by CLI NAT diagnostics.
- `src/transport/SignalingClient.*`: WinHTTP client for the optional HTTP room server diagnostics.
- `src/ui/`: optional Qt Widgets desktop control UI. Live sessions and normal display/audio-device discovery use the concrete session API; helper diagnostics still use short-lived CLI commands where useful.
- `signaling-worker/`: optional Cloudflare Worker TypeScript project for room membership and UDP candidate exchange. It is signaling only; media remains direct native UDP.
- `scripts/install-dev-deps.ps1`: Windows bootstrap script for MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm dependencies.

## Build Shape

- Debug preset output: `build/debug/ScreenShare.exe`.
- Release preset output: `build/release/ScreenShare.exe`.
- `ScreenShareCore` is a static library target containing the reusable native engine modules.
- `ScreenShareSessionRuntime` is a static library target containing the concrete session API and runtime-backed Share/Watch execution; `ScreenShare.exe` is a tiny main linked against it.
- If Qt 6 Widgets and Svg are available at configure time, builds also output `ScreenShareUi.exe`.
- Normal default builds create portable zip packages; see `agents/packaging.md`.
