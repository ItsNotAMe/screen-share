# Release And Update Publishing

ScreenShare updates are published as GitHub Release assets:

- `ScreenShare-release-windows-x64.zip`
- `ScreenShare-Setup-1.0.0-windows-x64.exe`
- `screenshare-update.json`

The desktop app checks the HTTPS manifest from the latest GitHub Release. It only offers an update
when the manifest version is newer than the compiled app version and the downloaded package matches
the manifest SHA-256 hash. Installed copies select the Setup asset; portable copies select the ZIP.

## Build

One-time setup on your Windows release account (the existing encrypted key and
public key stay in `%USERPROFILE%/.screenshare-release`):

```powershell
.\scripts\set-update-signing-secret.ps1
```

Enter the key's passphrase locally. It is verified, then stored with Windows
DPAPI protection and an account-only file ACL. Never put it in chat, source
control or command-line arguments. Keep the original encrypted key backed up.
Another Windows account/machine requires its own setup.

For each release, commit the intended changes and run:

```powershell
.\scripts\build-release.ps1
```

This builds and tests the application, packages Setup/ZIP, checks the relocated
UI, fetches hash-pinned Qt sources, and signs/verifies both update assets using
the stored credential. It does not publish. After merging and building from the
clean merged `main` commit, publish with:

```powershell
.\scripts\publish-release.ps1 -VerifyOnly
.\scripts\publish-release.ps1
```

Publication checks the recorded commit, pinned signing key, signatures, package
hashes and Qt source hashes, then uploads a draft and makes it the latest release.
Existing releases are never overwritten. The lower-level commands follow for
manual builds and offline signing.

```powershell
cmake --preset release
cmake --build --preset release
```

For the controller-enabled release, run the pinned runtime fetcher and build the elevated Setup
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
  -Version 1.0.0 `
  -ZipPath .\build\release\ScreenShare-release-windows-x64.zip `
  -InstallerPath .\build\release\ScreenShare-Setup-1.0.0-windows-x64.exe `
  -OutputPath .\build\release\screenshare-update.json `
  -Channel stable `
  -Notes "ScreenShare v1.0.0"
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

The Setup executable should also be Authenticode-signed before a public release. For a release without an Authenticode certificate, the signed update manifest still protects downloaded Setup bytes, but the initial Setup
download displays Windows' Unknown Publisher warning.

## Publish

For 1.0.0, use `docs/release-1.0.0.md` as the release notes and upload the
three Qt 6.10.3 source archives listed in `third_party/qt-6.10.3/SOURCE-SHA256.txt`
alongside the binary assets. Verify their hashes before upload. Both packages
include Qt license texts, upstream SBOMs, and replacement-library instructions.
The update manifest must be signed after the final packages are built; any
subsequent package change requires regenerating and signing it again.

```powershell
gh release create v1.0.0 `
  .\build\release\ScreenShare-release-windows-x64.zip `
  .\build\release\ScreenShare-Setup-1.0.0-windows-x64.exe `
  .\build\release\screenshare-update.json `
  --latest `
  --title "ScreenShare v1.0.0" `
  --notes-file .\docs\release-1.0.0.md
```

For an existing tag/release, replace `create` with `upload --clobber`.
