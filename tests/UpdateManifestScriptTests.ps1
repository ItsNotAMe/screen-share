param(
    [Parameter(Mandatory = $true)]
    [string]$GeneratorScript
)

$ErrorActionPreference = 'Stop'
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('screenshare-manifest-test-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Path $testDirectory | Out-Null
    $zipPath = Join-Path $testDirectory 'ScreenShare-test.zip'
    $installerPath = Join-Path $testDirectory 'ScreenShare-Setup-test.exe'
    $manifestPath = Join-Path $testDirectory 'screenshare-update.json'
    [IO.File]::WriteAllBytes($zipPath, [byte[]](1, 2, 3, 4))
    [IO.File]::WriteAllBytes($installerPath, [byte[]](5, 6, 7, 8, 9))
    $portableSignature = [Convert]::ToBase64String([byte[]]::new(64))
    $installerSignatureBytes = [byte[]]::new(64)
    $installerSignatureBytes[63] = 1
    $installerSignature = [Convert]::ToBase64String($installerSignatureBytes)

    & $GeneratorScript `
        -Version '9.8.7' `
        -ZipPath $zipPath `
        -InstallerPath $installerPath `
        -OutputPath $manifestPath `
        -Repository 'example/screenshare' `
        -Signature $portableSignature `
        -InstallerSignature $installerSignature

    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    if (-not $manifest.assets.portableZip -or -not $manifest.assets.windowsInstaller) {
        throw 'Both portableZip and windowsInstaller assets are required.'
    }
    if ($manifest.assets.portableZip.url -notlike 'https://github.com/example/screenshare/*') {
        throw 'Portable asset URL was generated incorrectly.'
    }
    if ($manifest.assets.windowsInstaller.url -notlike 'https://github.com/example/screenshare/*') {
        throw 'Installer asset URL was generated incorrectly.'
    }
    if ($manifest.assets.portableZip.sha256 -ne (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Portable asset hash is incorrect.'
    }
    if ($manifest.assets.windowsInstaller.sha256 -ne (Get-FileHash $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Installer asset hash is incorrect.'
    }
    if ($manifest.assets.portableZip.signature -ne $portableSignature -or $manifest.signature -ne $portableSignature) {
        throw 'Portable signature must be stored on the asset and at the legacy manifest root.'
    }
    if ($manifest.assets.windowsInstaller.signature -ne $installerSignature) {
        throw 'Installer signature was not stored independently.'
    }
} finally {
    Remove-Item -LiteralPath $testDirectory -Recurse -Force -ErrorAction SilentlyContinue
}
