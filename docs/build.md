# Native Windows build

## Prerequisites

Use Windows x64, Visual Studio C++ Build Tools with native CMake, Python 3.11+,
Git and Node.js for local Worker integration tests. The pinned compiler is
clang-cl from the WebRTC checkout; MSYS/MinGW binaries are not interchangeable.
Qt Core, Network and WebSockets are required even for CLI-only builds. The UI
also uses Widgets and Svg.

Dependency pins live in `cmake/dependencies/webrtc-source.json` and
`cmake/dependencies/native-dependencies.json`. The adjacent `webrtc-build.patch`
sets the MSVC dynamic CRT and exception/RTTI defaults. Its bytes and checksum are
part of the SDK identity; moving it must not alter its line endings. The pins
specify Qt 6.10.3 MSVC x64 and the matching Chromium/Clang/Windows SDK inputs.

## Clean build

No existing `build` directory or manually populated toolchain folder is required.
In VS Code, select `release` or `debug`, then configure/build normally. The presets
prepare missing native dependencies **before** CMake checks for the compiler.
The first configure downloads Qt/WebRTC and builds the selected WebRTC artifact;
it needs network access, substantial disk space and time. Subsequent builds reuse
that cache. CMake creates the output directory automatically.

In an x64 Visual Studio developer shell using its native CMake:

```powershell
cmake --preset release
cmake --build --preset release
cmake --build build/release --target package-portable
```

- `.deps/` contains downloaded Qt, Vulkan headers, WebRTC sources, the pinned
  Clang/Ninja toolchain, and Debug/Release WebRTC artifacts. It is ignored by Git.
- `build/` contains disposable application/test/package output. Deleting it does
  not remove the dependency cache. Deleting `.deps` intentionally triggers setup again.
- Visual Studio Build Tools, the pinned Windows SDK, Git, Python and Node remain
  installed prerequisites; bootstrap does not install system software or drivers.

`debug`, `native-debug` and `native-release` use the same cache. Debug and Release
WebRTC libraries are separate. A failed/interrupted setup can be retried by
configuring again; already completed stages are reused. Concurrent preparation
fails clearly rather than modifying the same checkout from two processes.

To prepare explicitly or verify availability without downloads:

```powershell
./scripts/prepare-dependencies.ps1 -Configuration Release
./scripts/prepare-dependencies.ps1 -Configuration Release -CheckOnly
```

Use `-DSCREENSHARE_BOOTSTRAP_DEPENDENCIES=OFF` during configure to disable automatic
preparation. This rejects missing dependencies with setup instructions. Other
CMake dependencies, such as the hash-pinned ViGEm client, still need provisioning
for a completely offline application build.

A custom cache is supported with
`-DSCREENSHARE_DEPENDENCY_ROOT=C:/path/to/cache` on a fresh configure, or
`-DependencyRoot C:/path/to/cache` when preparing manually. Existing cached compiler
paths are not silently rewritten: use a fresh CMake output directory when moving
an existing cache or changing toolchains. The lower-level WebRTC sync/build scripts
accept their WebRTC subdirectory as `-DependencyRoot` (for example `.deps/webrtc`).

The existing `run-webrtc-proof.ps1` runner imports the Visual Studio environment
for its process and uses the same automatic setup through the presets.
Do not use MSYS CRT headers or MSYS CMake; Vulkan headers are under
`.deps/Vulkan-Headers`. Close the running app before replacing its executable;
individual test targets can be built without relinking it.

## Relocatable WebRTC SDK

```powershell
python scripts/webrtc-sdk.py export .deps/webrtc/checkout/src/out/screenshare-release .deps/webrtc-sdk-cache
python scripts/webrtc-sdk.py verify <sdk-directory>
./scripts/run-webrtc-proof.ps1 -Application -Configuration release -ArtifactDirectory <sdk-directory> -BuildDirectory build/sdk-app-release -ViGEmSourceDirectory <existing-vigem-source> -Package
```

SDK export is offline and content-addressed. Verification checks the complete
inventory, compiler, source revision, GN arguments, patch, configuration, CRT,
MSVC and Windows SDK identity. For a supplied SDK/toolchain, provide matching compiler and Qt paths explicitly;
bootstrap refuses to replace incomplete custom paths. An SDK is not a substitute for Qt or the compiler,
and checksums do not authenticate an untrusted download. Export Debug separately.
`--resume <export-directory-name>` resumes interrupted staging with verification.

For CLI-only builds use `-Application -NoUi` in a separate build directory;
that option persists in its cache. Use `-BuildDirectory` to select the directory.

Native packaging stages matching Qt plugins, MSVC runtime DLLs, WebRTC notices
and Qt SBOMs, and rejects unresolved dependencies. This does not establish
fresh-machine installer compatibility or complete distribution obligations.
See [testing](testing.md), [limitations](known-limitations.md) and [release](release.md).
