[CmdletBinding()]
param(
    [string]$DependencyRoot,
    [switch]$RunHooks
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $repoRoot 'cmake/dependencies/webrtc-source.json') -Raw | ConvertFrom-Json
if (-not $DependencyRoot) { $DependencyRoot = Join-Path $repoRoot '.deps/webrtc' }
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$depotRoot = Join-Path $DependencyRoot 'depot_tools'
$checkoutRoot = Join-Path $DependencyRoot 'checkout'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

foreach ($revision in @($lock.commit, $lock.depotToolsCommit)) {
    if ($revision -notmatch '^[a-f0-9]{40}$') { throw 'Expected a full pinned Git revision' }
}
New-Item -ItemType Directory -Force -Path $DependencyRoot | Out-Null
if (-not (Test-Path -LiteralPath $depotRoot)) {
    Invoke-Checked git @('clone', '--depth', '1', $lock.depotToolsRepository, $depotRoot)
}
$depotChanges = & git -C $depotRoot status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $depotChanges) { throw 'depot_tools must be a clean Git checkout' }
Invoke-Checked git @('-C', $depotRoot, 'fetch', '--depth', '1', 'origin', $lock.depotToolsCommit)
Invoke-Checked git @('-C', $depotRoot, 'checkout', '--detach', $lock.depotToolsCommit)

# Disable depot_tools self-updates; Chromium downloads pinned Clang through DEPS.
# Use installed Windows build tools rather than Google's restricted toolchain.
$savedPath = $env:PATH
$savedUpdate = $env:DEPOT_TOOLS_UPDATE
$savedWinToolchain = $env:DEPOT_TOOLS_WIN_TOOLCHAIN
try {
    $env:PATH = "$depotRoot;$savedPath"
    $env:DEPOT_TOOLS_UPDATE = '0'
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
    Invoke-Checked (Join-Path $depotRoot 'bootstrap/win_tools.bat') @()
    New-Item -ItemType Directory -Force -Path $checkoutRoot | Out-Null
    Push-Location $checkoutRoot
    try {
        $gclient = Join-Path $depotRoot 'gclient.bat'
        if (-not (Test-Path -LiteralPath '.gclient')) {
            Invoke-Checked $gclient @('config', $lock.repository)
        }
        Invoke-Checked $gclient @('sync', '--no-history', '--nohooks', '--revision', "src@$($lock.commit)")
        $actualRevision = & git -C src rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $actualRevision -ne $lock.commit) { throw 'WebRTC revision mismatch' }
        if ($RunHooks) { Invoke-Checked $gclient @('runhooks') }
    } finally { Pop-Location }
} finally {
    $env:PATH = $savedPath
    $env:DEPOT_TOOLS_UPDATE = $savedUpdate
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = $savedWinToolchain
}
Write-Output "Pinned source checkout: $checkoutRoot/src"
