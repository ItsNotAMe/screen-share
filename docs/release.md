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

## Generate And Sign Manifest

Generate the manifest once to calculate the package URL and SHA-256 signing fields:

```powershell
.\scripts\create-update-manifest.ps1 `
  -Version 0.2.3 `
  -ZipPath .\build\release\ScreenShare-release-windows-x64.zip `
  -OutputPath .\build\release\screenshare-update.json `
  -Notes "Auto-update support","Portable Windows package"
```

Sign the manifest with the encrypted offline key. The helper constructs the exact
message, verifies the result against the public key, converts the signature to raw
`R||S`, and writes its base64 form into the manifest:

```powershell
.\scripts\sign-update-manifest.ps1 `
  -ManifestPath .\build\release\screenshare-update.json `
  -PrivateKeyPath "$env:USERPROFILE\.screenshare-release\screenshare-update.key" `
  -PublicKeyPath "$env:USERPROFILE\.screenshare-release\screenshare-update-public.der"
```

Never publish the unsigned intermediate manifest. The desktop updater rejects it by design.

## Publish

```powershell
gh release create v0.2.3 `
  .\build\release\ScreenShare-release-windows-x64.zip `
  .\build\release\screenshare-update.json `
  --title "ScreenShare 0.2.3" `
  --notes "Multi-viewer rooms now isolate each viewer's send queue, recovery, health, and disconnect lifecycle."
```

For an existing tag/release, replace `create` with `upload --clobber`.
