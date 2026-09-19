# Repo Map

This is a native Windows C++ screen-sharing prototype. Native engine code lives
under `backend/`; desktop UI, CLI and updater clients live under `frontend/`.
The separately deployed service remains in `signaling-worker/`. Durable repo
memory lives under `agents/`, with `.codex/README.md` as a local-tooling entrypoint.

## Source Layout

- `backend/api/ScreenShareAPI.h` and `backend/api/ScreenShareAPI.cpp`: concrete `screenshare::ScreenShareSession` API facade for Share/Watch start, stop, runtime settings, status snapshots, events, display discovery, and audio-device discovery.
- `frontend/cli/ScreenShareCLI.*`: CLI runner for command-line parsing, help text, diagnostic command dispatch, report wrapping, and calls into shared runtime execution.
- `backend/runtime/ScreenShareRunContext.h`, `backend/runtime/ScreenShareSessionRunner.h`, and `backend/runtime/ScreenShareSessionRunner.cpp`: typed runtime-backed Share/Watch session runner plumbing used by the concrete API without including the CLI header.
- `backend/runtime/ScreenShareRuntimeOptions.h`: shared runtime option model and runtime constants used while CLI parsing and reusable session execution are being separated.
- `backend/runtime/ScreenShareSessionOptions.*`: shared typed Share/Watch config-to-runtime-options conversion, session/access-code validation helpers, and NAT target helpers used by both CLI presets and the concrete session API path.
- `backend/runtime/ScreenShareRuntimeExecution.cpp`: shared normal runtime execution for capture/send, receive/preview/audio playback, standalone audio capture, live signaling setup, adaptation policy, and typed Share/Watch execution entrypoints.
- `backend/runtime/ScreenShareRuntimeSupport.*`: shared session IDs/fingerprints, stdout/stderr capture, saved-report zip writing, and argv helpers used by both the CLI runner and typed session runner.
- `backend/runtime/ScreenShareRuntimeInternal.h`: private declarations shared only inside the session runtime implementation; not a public UI/backend API surface.
- `backend/core/ScreenShareSession.*`: shared session data types and helpers used by the API, CLI, and UI.
- `frontend/cli/ScreenShareMain.cpp`: tiny executable entry point that calls the CLI runner.
- `backend/core/`: shared session data types, legacy typed Share/Watch command-preview builders, runtime-control interfaces, and native core-library entry points that are intended to be used by both the CLI and UI.
- `backend/capture/`: Windows Graphics Capture and DXGI Desktop Duplication capture path, HDR/scRGB handling, GPU scaling/NV12 generation.
- `backend/codec/`: H.264 file encoder, H.264 stream encoder/decoder, H.264 bitstream helpers, encoder probing.
- `backend/transport/`: UDP sender/receiver, media packet format, fragmentation/reassembly, receiver feedback snapshots.
- `backend/audio/`: WASAPI capture/playback and Opus encode/decode.
- `backend/render/`: native Win32/D3D11 receiver preview window and GPU NV12 presentation.
- `backend/video/`: CPU NV12 conversion used for diagnostics and fallback paths.
- `backend/transport/LanDiscovery.*`: opt-in LAN receiver discovery helper used by CLI and Qt UI.
- `backend/transport/StunClient.*`: standalone STUN Binding Request helper used by CLI NAT diagnostics.
- `backend/transport/SignalingClient.*`: WinHTTP client for the optional HTTP room server diagnostics.
- `frontend/ui/`: optional Qt Widgets desktop control UI. Live sessions and normal display/audio-device discovery use the concrete session API; helper diagnostics still use short-lived CLI commands where useful.
- `signaling-worker/`: Cloudflare Worker TypeScript room service, including authenticated v2 WebRTC signaling and the legacy protocol. Media never travels through the Worker; normal clients still use the legacy runtime while v2 headless integration uses WebRTC.
- `scripts/install-dev-deps.ps1`: Windows bootstrap script for MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm dependencies.

## Build Shape

- Backend v2 build proof adds `native-debug` / `native-release` presets with clang-cl, MSVC Qt and pinned WebRTC/Opus; output is `build/native-debug` / `build/native-release`. Use `scripts/run-webrtc-proof.ps1 -Application -Configuration debug|release` to initialize the correct native tools. See `docs/build.md`.
- `tools/webrtc-proof/` contains MF-through-WebRTC video/data-channel, encoder/decoder lifecycle and primitive MF probes. `run-webrtc-proof.ps1 -Hardware` enables GPU input, hardware lifecycle/fallback/quarantine/cancellation and texture/readback tests. Private factories and ownership contracts live in `backend/media/webrtc/`. It does not replace application media routing yet.

- Debug preset output: `build/debug/ScreenShare.exe`.
- Release preset output: `build/release/ScreenShare.exe`.
- `ScreenShareCore` is a static library target containing the reusable native engine modules.
- `ScreenShareAPI` is a static library target containing the concrete session API and runtime-backed Share/Watch execution without CLI parsing.
- `ScreenShare.exe` compiles the CLI parser/entrypoint and links `ScreenShareAPI`.
- `ScreenShareUi.exe` links `ScreenShareAPI` directly when Qt 6 Widgets and Svg are available.
- If Qt 6 Widgets and Svg are available at configure time, builds also output `ScreenShareUi.exe`.
- Normal default builds create portable zip packages; see `agents/packaging.md`.
