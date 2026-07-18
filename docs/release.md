# Release And Update Publishing

ScreenShare updates are published as GitHub Release assets:

- `ScreenShare-release-windows-x64.zip`
- `ScreenShare-Setup-0.3.1-windows-x64.exe`
- `screenshare-update.json`

The desktop app checks the HTTPS manifest from the latest GitHub Release. It only offers an update
when the manifest version is newer than the compiled app version and the downloaded package matches
the manifest SHA-256 hash. Installed copies select the Setup asset; portable copies select the ZIP.

## Build

```powershell
cmake --preset release
cmake --build --preset release
```

For the controller-enabled prerelease, run the pinned runtime fetcher and build the elevated Setup
package. Installation and driver updates happen only inside this setup flow; normal ScreenShare
launches perform a read-only availability probe.

```powershell
$driverSetup = .\scripts\fetch-controller-runtime.ps1
cmake --preset release `
  -DSCREENSHARE_BUILD_INSTALLER=ON `
  -DSCREENSHARE_CONTROLLER_DRIVER_SETUP="$driverSetup" `
  -DSCREENSHARE_INNO_SETUP_COMPILER="C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
cmake --build --preset release
```

## Generate And Sign Manifest

Generate the manifest once to calculate the package URL and SHA-256 signing fields:

```powershell
.\scripts\create-update-manifest.ps1 `
  -Version 0.3.1 `
  -ZipPath .\build\release\ScreenShare-release-windows-x64.zip `
  -InstallerPath .\build\release\ScreenShare-Setup-0.3.1-windows-x64.exe `
  -OutputPath .\build\release\screenshare-update.json `
  -Channel prerelease `
  -Notes "Fix installed UI report paths","Save relative reports under LocalAppData"
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

The Setup executable should also be Authenticode-signed before a public release. For an internal
prerelease, the signed update manifest still protects downloaded Setup bytes, but the initial Setup
download displays Windows' Unknown Publisher warning.

## Publish

```powershell
gh release create v0.3.1 `
  .\build\release\ScreenShare-release-windows-x64.zip `
  .\build\release\ScreenShare-Setup-0.3.1-windows-x64.exe `
  .\build\release\screenshare-update.json `
  --prerelease `
  --title "ScreenShare 0.3.1 prerelease" `
  --notes "Fixes room startup from Program Files by saving relative UI reports under LocalAppData."
```

For an existing tag/release, replace `create` with `upload --clobber`.
