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
    [string[]]$Notes = @("Portable ScreenShare update.")
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

$json = $manifest | ConvertTo-Json -Depth 6
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
Set-Content -LiteralPath $OutputPath -Value $json -Encoding UTF8
Write-Host "Wrote $OutputPath"
Write-Host "SHA256 $hash"
