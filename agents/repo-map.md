# Repo Map

This is a native Windows C++ screen-sharing prototype. Public code lives under `src/`; durable repo memory lives under `agents/`, with `.codex/README.md` as a local-tooling entrypoint.

## Source Layout

- `src/app/main.cpp`: command-line parsing, top-level sender/receiver loops, adaptation policy, stats printing, logging.
- `src/capture/`: Windows Graphics Capture and DXGI Desktop Duplication capture path, HDR/scRGB handling, GPU scaling/NV12 generation.
- `src/codec/`: H.264 file encoder, H.264 stream encoder/decoder, H.264 bitstream helpers, encoder probing.
- `src/transport/`: UDP sender/receiver, media packet format, fragmentation/reassembly, receiver feedback snapshots.
- `src/audio/`: WASAPI capture/playback and Opus encode/decode.
- `src/render/`: native Win32/D3D11 receiver preview window and GPU NV12 presentation.
- `src/video/`: CPU NV12 conversion used for diagnostics and fallback paths.
- `src/transport/LanDiscovery.*`: opt-in LAN receiver discovery helper used by CLI and Qt UI.
- `src/transport/StunClient.*`: standalone STUN Binding Request helper used by CLI NAT diagnostics.
- `src/ui/`: optional Qt Widgets desktop control UI that launches the CLI engine.
- `signaling-worker/`: optional Cloudflare Worker TypeScript project for room membership and UDP candidate exchange. It is signaling only; media remains direct native UDP.
- `scripts/install-dev-deps.ps1`: Windows bootstrap script for MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm dependencies.

## Build Shape

- Debug preset output: `build/debug/ScreenShare.exe`.
- Release preset output: `build/release/ScreenShare.exe`.
- If Qt 6 Widgets is available at configure time, builds also output `ScreenShareUi.exe`.
- Normal default builds create portable zip packages; see `agents/packaging.md`.
