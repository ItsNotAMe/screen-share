param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$ZipPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$Channel = "stable",
    [string]$Repository = "ItsNotAMe/screen-share",
    [string]$AssetName,
    [string[]]$Notes = @("Portable ScreenShare update."),
    [string]$Signature
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

$manifest = [ordered]@{
    version = $Version
    channel = $Channel
    platform = "windows-x64"
    notes = $Notes
    assets = [ordered]@{
        portableZip = [ordered]@{
            url = $downloadUrl
            sha256 = $hash
            size = $size
        }
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
    $manifest["signature"] = $Signature
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
if (-not $Signature) {
    Write-Warning "Manifest is unsigned and must not be published. Sign the three fields above, then rerun with -Signature."
}
