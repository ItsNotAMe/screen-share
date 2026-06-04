# Release And Update Publishing

ScreenShare updates are published as GitHub Release assets:

- `ScreenShare-release-windows-x64.zip`
- `screenshare-update.json`

The desktop app checks the HTTPS manifest from the latest GitHub Release. It only offers an update
when the manifest version is newer than the compiled app version and the downloaded zip matches the
manifest SHA-256 hash.

## Build

```powershell
cmake --preset release
cmake --build --preset release
```

## Generate Manifest

```powershell
.\scripts\create-update-manifest.ps1 `
  -Version 0.1.0 `
  -ZipPath .\build\release\ScreenShare-release-windows-x64.zip `
  -OutputPath .\build\release\screenshare-update.json `
  -Notes "Auto-update support","Portable Windows package"
```

## Publish

```powershell
gh release create v0.1.0 `
  .\build\release\ScreenShare-release-windows-x64.zip `
  .\build\release\screenshare-update.json `
  --title "ScreenShare 0.1.0" `
  --notes "Initial portable release with auto-update support."
```

For an existing tag/release, replace `create` with `upload --clobber`.
