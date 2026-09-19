[CmdletBinding()]
param([switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $record = Get-Content build/release/release-build.json -Raw | ConvertFrom-Json
    $head = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $record.commit -ne $head -or (& git status --porcelain)) { throw 'Publish only the clean commit recorded by build-release.ps1.' }
    & "$PSScriptRoot/verify-update-manifest.ps1" -ManifestPath build/release/screenshare-update.json
    $manifest = Get-Content build/release/screenshare-update.json -Raw | ConvertFrom-Json
    if ($record.version -ne $manifest.version) { throw 'Release record and signed manifest versions differ.' }
    $assets = @('build/release/ScreenShare-release-windows-x64.zip', "build/release/ScreenShare-Setup-$($record.version)-windows-x64.exe", 'build/release/screenshare-update.json')
    foreach ($line in Get-Content third_party/qt-6.10.3/SOURCE-SHA256.txt) {
        $hash, $name = $line -split '\s+',2
        $path = "build/release/qt-sources/$name"
        if ((Get-FileHash $path).Hash -ine $hash) { throw "Qt source mismatch: $name" }
        $assets += $path
    }
    if ($VerifyOnly) { Write-Host 'Release assets verified; nothing published.'; return }
    if ((& git branch --show-current).Trim() -ne 'main') { throw 'Merge the reviewed release branch before publishing from main.' }
    & git fetch origin main
    if ($LASTEXITCODE -ne 0 -or (& git rev-parse origin/main).Trim() -ne $head) { throw 'Local main does not match origin/main.' }
    $tag = "v$($record.version)"
    $notes = "docs/release-$($record.version).md"
    if (-not (Test-Path $notes)) { throw 'Versioned release notes are missing.' }
    # Create a draft with every asset before exposing it to the stable updater.
    & gh release create $tag @assets --repo ItsNotAMe/screen-share --target $head --draft --title "ScreenShare $tag" --notes-file $notes
    if ($LASTEXITCODE -ne 0) { throw 'Draft creation failed. Inspect GitHub before retrying; existing releases are never overwritten.' }
    & gh release edit $tag --repo ItsNotAMe/screen-share --draft=false --latest
    if ($LASTEXITCODE -ne 0) { throw 'The complete release remains a draft; publication failed.' }
    Write-Host "Published https://github.com/ItsNotAMe/screen-share/releases/tag/$tag"
} finally { Pop-Location }
