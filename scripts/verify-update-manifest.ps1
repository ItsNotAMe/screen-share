[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ManifestPath,
    [string]$PublicKeyPath = "$env:USERPROFILE/.screenshare-release/screenshare-update-public.der"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/UpdateSigning.ps1"
Assert-PinnedUpdatePublicKey $PublicKeyPath
$manifestFile = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if ($manifest.version -notmatch '^\d+\.\d+\.\d+$' -or $manifest.channel -ne 'stable') { throw 'Invalid stable update version/channel.' }
[byte[]]$der = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PublicKeyPath).Path)
# BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32-byte coordinates, then raw X||Y.
[byte[]]$blob = @(0x45,0x43,0x53,0x31,32,0,0,0) + $der[27..90]
$key = [Security.Cryptography.CngKey]::Import($blob,[Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
$ecdsa = [Security.Cryptography.ECDsaCng]::new($key)
try {
    foreach ($kind in @('portableZip','windowsInstaller')) {
        $asset = $manifest.assets.$kind
        $name = if ($kind -eq 'portableZip') { 'ScreenShare-release-windows-x64.zip' } else { "ScreenShare-Setup-$($manifest.version)-windows-x64.exe" }
        $url = "https://github.com/ItsNotAMe/screen-share/releases/download/v$($manifest.version)/$name"
        if ($asset.url -cne $url -or $asset.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid $kind URL/hash." }
        [byte[]]$signature = [Convert]::FromBase64String($asset.signature)
        [byte[]]$message = [Text.Encoding]::UTF8.GetBytes("$($manifest.version)`n$url`n$($asset.sha256)")
        if ($signature.Length -ne 64 -or -not $ecdsa.VerifyData($message,$signature,[Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "Invalid $kind signature." }
        $package = Join-Path (Split-Path $manifestFile) $name
        if ((Get-FileHash -LiteralPath $package).Hash -ine $asset.sha256 -or (Get-Item $package).Length -ne $asset.size) { throw "The $kind package changed after signing." }
    }
    if ($manifest.signature -cne $manifest.assets.portableZip.signature) { throw 'Legacy signature does not match the signed portable asset.' }
    Write-Host 'Both update signatures and package hashes verified against the pinned release key.'
} finally { $ecdsa.Dispose(); $key.Dispose() }
