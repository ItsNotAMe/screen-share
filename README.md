# Screen Share

Native Windows screen sharing with H.264 video, WASAPI audio, WebRTC transport,
and a Qt desktop interface. The modular room backend is now the default in both
the UI and CLI. The old UDP sharing commands and v1 room links are retired.

The current personal-use build connects to the existing isolated v2 service:
`https://screenshare-signaling-v2.bit-yeet.workers.dev` (ten-room cap).
This client cutover does not deploy, migrate or replace the v1 service.
Override the origin with `--signal-server HTTPS_ORIGIN`, or set
`SCREENSHARE_DEFAULT_ROOM_ORIGIN` when configuring a build.

## Run

Open `ScreenShareUi.exe`, choose Create Room or Join Room, and share the new
room ID or link. Room lists update through a persistent subscription.
Nicknames, capture/audio selections, stream settings, diagnostics and explicit
host-approved remote input use the shared backend.

CLI examples:

```powershell
.\ScreenShare.exe --create-room --name "Game night" --nickname Host --display 0 --preset gaming
.\ScreenShare.exe --join-room ROOM_ID --nickname Viewer
.\ScreenShare.exe --join-room ROOM_ID --decoder software
.\ScreenShare.exe --help
```

Use the software decoder option on the tested laptop where hardware graphics
resource behavior remains unqualified. The GameSir Bluetooth field checks and
physical-reader regrant pass; multi-controller virtual-driver allocation remains
a known limitation. See [known limitations](docs/known-limitations.md) and
[controller support](docs/controller-support.md).

## Build and package

The application requires Windows x64, the pinned clang-cl/WebRTC SDK, a matching
MSVC/Windows SDK environment, and Qt 6 Core/Network/WebSockets/Widgets/Svg.
Follow [native setup and SDK instructions](docs/build.md). The earlier
MinGW-only application path is retired; debug/release presets now use the native
toolchain. Use a fresh build directory if an existing cache used MinGW.

In VS Code, select the `release` or `debug` CMake configure preset. Workspace
settings select the pinned Build Tools installation's native CMake and enable
the Visual Studio developer environment; the presets request x64
host/target tools without passing unsupported architecture flags to Ninja.
If an earlier configure reported `rc` missing or `CMAKE_MT-NOTFOUND`, run
**CMake: Delete Cache and Reconfigure** after reloading the window. Command-line
builds still need an x64 Visual Studio developer shell. If Build Tools is installed
elsewhere, adjust `cmake.cmakePath` in `.vscode/settings.json` to its bundled CMake;
do not use the MSYS2 CMake for this native toolchain. See Microsoft's
[CMake Tools preset guidance](https://github.com/microsoft/vscode-cmake-tools/blob/main/docs/cmake-presets.md).

No existing `build` folder is needed. Configure prepares missing pinned dependencies
in the ignored `.deps/` cache before checking the compiler. The first run downloads
and compiles them; later builds reuse them even after deleting `build/`.
Visual Studio Build Tools, the pinned Windows SDK, Git and Python must be installed.

From a native Visual Studio developer shell:

```powershell
cmake --preset native-release
cmake --build --preset native-release
cmake --build build/native-release --target package-portable
```

The equivalent Debug preset is `native-debug`. The portable archive is produced
in the selected build directory. SDK-only/relocated build instructions and
verification are in [build instructions](docs/build.md).
The hash-pinned ViGEm client is built from source; the application never installs
or repairs drivers during normal startup. Installer publishing and signed update
manifests remain separate release actions: [release instructions](docs/release.md).

## Architecture

- [backend/](backend/README.md): room/session API, capture, codecs, audio, input,
  presentation and platform services.
- [frontend/](frontend/README.md): Qt UI, CLI and updater.
- `signaling-worker/`: admission, room/directory updates and signaling; media
  travels between peers through WebRTC, not through the Worker.
- Historical custom UDP transport and runner are isolated in diagnostic build
  targets for the reproducible legacy comparison. Shipping entry points do not
  link or expose them.

The modular path uses owned frames, bounded queues, independent per-viewer
adaptation, explicit input consent and asynchronous teardown.
[Measured comparisons](docs/performance.md) record benefits and tradeoffs.
Remaining qualification is deferred; [the maintenance backlog](agents/todo.md)
tracks unresolved work and release preparation.

## Documentation

- [Current usage](docs/usage.md)
- [CLI configuration and controls](docs/cli.md)
- [Headless testing](docs/testing.md)
- [Cutover and upgrade behavior](docs/usage.md)
- [Controller support](docs/controller-support.md)
- [Deferred work](docs/known-limitations.md)
