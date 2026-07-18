param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$ZipPath,

    [string]$InstallerPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$Channel = "stable",
    [string]$Repository = "ItsNotAMe/screen-share",
    [string]$AssetName,
    [string]$InstallerAssetName,
    [string[]]$Notes = @("Portable ScreenShare update."),
    [string]$Signature,
    [string]$InstallerSignature
)

$ErrorActionPreference = "Stop"

$resolvedZip = Resolve-Path -LiteralPath $ZipPath
if (-not $AssetName) {
    $AssetName = Split-Path -Leaf $resolvedZip
}

$hash = (Get-FileHash -LiteralPath $resolvedZip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $resolvedZip).Length
$tag = "v$Version"
$downloadUrl = "https://github.com/$Repository/releases/download/$tag/$AssetName"

$portableAsset = [ordered]@{
    url = $downloadUrl
    sha256 = $hash
    size = $size
}

$manifest = [ordered]@{
    version = $Version
    channel = $Channel
    platform = "windows-x64"
    notes = $Notes
    assets = [ordered]@{
        portableZip = $portableAsset
    }
}

if ($Signature) {
    try {
        $signatureBytes = [Convert]::FromBase64String($Signature)
    } catch {
        throw "Signature must be valid base64: $($_.Exception.Message)"
    }
    if ($signatureBytes.Length -ne 64) {
        throw "Signature must decode to exactly 64 bytes (raw ECDSA P-256 R||S); got $($signatureBytes.Length)"
    }
    $portableAsset["signature"] = $Signature
    # ScreenShare 0.2.3 and earlier read the portable signature from the
    # manifest root. Keep that copy while newer clients use per-asset
    # signatures.
    $manifest["signature"] = $Signature
}

if ($InstallerPath) {
    $resolvedInstaller = Resolve-Path -LiteralPath $InstallerPath
    if (-not $InstallerAssetName) {
        $InstallerAssetName = Split-Path -Leaf $resolvedInstaller
    }
    $installerHash = (Get-FileHash -LiteralPath $resolvedInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
    $installerSize = (Get-Item -LiteralPath $resolvedInstaller).Length
    $installerUrl = "https://github.com/$Repository/releases/download/$tag/$InstallerAssetName"
    $installerAsset = [ordered]@{
        url = $installerUrl
        sha256 = $installerHash
        size = $installerSize
    }
    if ($InstallerSignature) {
        try {
            $installerSignatureBytes = [Convert]::FromBase64String($InstallerSignature)
        } catch {
            throw "InstallerSignature must be valid base64: $($_.Exception.Message)"
        }
        if ($installerSignatureBytes.Length -ne 64) {
            throw "InstallerSignature must decode to exactly 64 bytes; got $($installerSignatureBytes.Length)"
        }
        $installerAsset["signature"] = $InstallerSignature
    }
    $manifest.assets["windowsInstaller"] = $installerAsset
} elseif ($InstallerSignature) {
    throw "InstallerSignature requires InstallerPath."
}

$json = $manifest | ConvertTo-Json -Depth 6
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
Set-Content -LiteralPath $OutputPath -Value $json -Encoding UTF8
Write-Host "Wrote $OutputPath"
Write-Host "SHA256 $hash"
Write-Host "Signing message fields:"
Write-Host "  version: $Version"
Write-Host "  package URL: $downloadUrl"
Write-Host "  SHA256: $hash"
if ($InstallerPath) {
    Write-Host "Installer signing message fields:"
    Write-Host "  version: $Version"
    Write-Host "  package URL: $installerUrl"
    Write-Host "  SHA256: $installerHash"
}
if (-not $Signature -or ($InstallerPath -and -not $InstallerSignature)) {
    Write-Warning "Manifest contains unsigned assets and must not be published. Run sign-update-manifest.ps1."
}
