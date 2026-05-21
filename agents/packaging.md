# Packaging Notes

MinGW builds need runtime DLLs beside the exe when moved to another computer. Current main/PR history statically links GCC/C++/pthread runtime by default and stages remaining DLLs such as Opus, UCRT redist files, and `d3dcompiler_47.dll` when available.

Portable zip packaging was merged separately from the live-streaming fixes in PR #46.

Desired packaging behavior:

- Normal CMake default/all build creates a zip.
- Explicit target remains available: `cmake --build --preset release --target package-portable`.
- Release zip path: `build/release/ScreenShare-release-windows-x64.zip`.
- Debug zip path: `build/debug/ScreenShare-debug-windows-x64.zip`.

When testing on another computer, delete old copied folders first. Stale DLLs next to the exe can cause Windows loader errors before the app starts.

Do not mix MSYS2 `mingw64` and `ucrt64` runtime DLLs. `ScreenShareUi.exe` is built from the UCRT toolchain, so Qt, ICU, `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, and `libwinpthread-1.dll` should all come from `C:/msys64/ucrt64/bin`.

Current validation:

- `cmake --build --preset debug` creates `build/debug/ScreenShare-debug-windows-x64.zip`.
- `cmake --build --preset release` creates `build/release/ScreenShare-release-windows-x64.zip`.
- The package contains `ScreenShare.exe`, staged runtime DLLs, `ScreenShare-runtime-dependencies.txt`, README, LICENSE, and `ScreenShare-package.txt`.
- When Qt is configured, the package also contains `ScreenShareUi.exe`, Qt DLLs, Qt plugin folders, and transitive MinGW/Qt dependency DLLs discovered during packaging.
- UI smoke check: run `ScreenShareUi.exe --self-test` from both the build output and staged package folder.
- The package step is attached to the default/all build only, so a normal build packages once. If VS Code's selected CMake target is `ScreenShare`, switch to `all` or `package-portable`.
