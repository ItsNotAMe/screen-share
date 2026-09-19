[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$DependencyRoot,
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DependencyRoot) { $DependencyRoot = Join-Path $repoRoot '.deps' }
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$native = Get-Content -LiteralPath (Join-Path $repoRoot 'cmake/dependencies/native-dependencies.json') -Raw | ConvertFrom-Json
$webrtc = Get-Content -LiteralPath (Join-Path $repoRoot 'cmake/dependencies/webrtc-source.json') -Raw | ConvertFrom-Json
$source = Join-Path $DependencyRoot 'webrtc/checkout/src'
$artifact = Join-Path $source "out/screenshare-$($Configuration.ToLowerInvariant())"
$patch = Join-Path $repoRoot 'cmake/dependencies/webrtc-build.patch'
$compiler = Join-Path $source 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe'

function Test-NativeDependencies {
    foreach ($component in @('Core', 'Network', 'WebSockets', 'Widgets', 'Svg')) {
        if (-not (Test-Path -LiteralPath (Join-Path $DependencyRoot "Qt/$($native.qtVersion)/msvc2022_64/lib/cmake/Qt6$component/Qt6${component}Config.cmake"))) { return $false }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $DependencyRoot 'Vulkan-Headers/include/vulkan/vulkan.h'))) { return $false }
    $revision = & git -C (Join-Path $DependencyRoot 'Vulkan-Headers') rev-parse HEAD 2>$null
    return $LASTEXITCODE -eq 0 -and $revision -eq $native.vulkanHeadersCommit
}
function Test-WebRtcArtifact {
    foreach ($file in @('screenshare-artifact.json', 'obj/webrtc.lib', 'args.gn')) {
        if (-not (Test-Path -LiteralPath (Join-Path $artifact $file))) { return $false }
    }
    foreach ($file in @('api/peer_connection_interface.h', 'third_party/ninja/ninja.exe',
        'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe', 'third_party/llvm-build/Release+Asserts/bin/lld-link.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $file))) { return $false }
    }
    try {
        $metadata = Get-Content -LiteralPath (Join-Path $artifact 'screenshare-artifact.json') -Raw | ConvertFrom-Json
        if ($metadata.schemaVersion -ne 1 -or $metadata.sourceRevision -ne $webrtc.commit -or
            $metadata.configuration -ne $Configuration -or $metadata.architecture -ne 'x64' -or
            [IO.Path]::GetFullPath($metadata.sourceDirectory) -ne [IO.Path]::GetFullPath($source)) { return $false }
        foreach ($entry in @(@($patch, $metadata.buildPatchSha256), @($compiler, $metadata.compilerSha256),
            @((Join-Path $artifact 'args.gn'), $metadata.gnArgumentsSha256), @((Join-Path $artifact 'obj/webrtc.lib'), $metadata.librarySha256))) {
            if ((Get-FileHash -LiteralPath $entry[0] -Algorithm SHA256).Hash -ne $entry[1]) { return $false }
        }
        return $true
    } catch { return $false }
}

if ($CheckOnly) {
    if (-not (Test-NativeDependencies) -or -not (Test-WebRtcArtifact)) {
        throw "Native dependencies are missing or stale. Run scripts/prepare-dependencies.ps1 -Configuration $Configuration -DependencyRoot `"$DependencyRoot`"."
    }
    Write-Output "Native $Configuration dependencies are ready: $DependencyRoot"
    return
}

# Serialize concurrent Debug/Release/IDE preparation. Never leave a readiness
# marker on failure; the next configure retries the incomplete stage.
New-Item -ItemType Directory -Force -Path $DependencyRoot | Out-Null
$preparationLock = $null
try {
    try { $preparationLock = [IO.File]::Open((Join-Path $DependencyRoot '.prepare.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch { throw 'Another dependency preparation is running. Wait for it to finish and configure again.' }
    if (-not (Test-NativeDependencies)) {
        Write-Host 'Preparing pinned Qt and Vulkan headers...'
        & (Join-Path $PSScriptRoot 'install-native-deps.ps1') -DependencyRoot $DependencyRoot
        if (-not (Test-NativeDependencies)) { throw 'Native dependency installation did not complete.' }
    }
    if (-not (Test-WebRtcArtifact)) {
        $webrtcRoot = Join-Path $DependencyRoot 'webrtc'
        $syncStamp = Join-Path $webrtcRoot '.screenshare-sync.sha256'
        $lockHash = (Get-FileHash -LiteralPath (Join-Path $repoRoot 'cmake/dependencies/webrtc-source.json') -Algorithm SHA256).Hash
        $sourceReady = (Test-Path -LiteralPath $syncStamp) -and
            ((Get-Content -LiteralPath $syncStamp -Raw).Trim() -eq $lockHash) -and
            (Test-Path -LiteralPath (Join-Path $source 'api/peer_connection_interface.h'))
        if (-not $sourceReady) {
            Write-Host 'Fetching pinned WebRTC sources. First-time setup can take a while...'
            & (Join-Path $PSScriptRoot 'sync-webrtc.ps1') -DependencyRoot $webrtcRoot
            $lockHash | Set-Content -LiteralPath $syncStamp -Encoding ascii
        }
        Write-Host "Building pinned WebRTC $Configuration ($Jobs jobs)..."
        & (Join-Path $PSScriptRoot 'build-webrtc.ps1') -Configuration $Configuration -DependencyRoot $webrtcRoot -Jobs $Jobs
        if (-not (Test-WebRtcArtifact)) { throw 'WebRTC preparation did not produce a valid artifact.' }
    }
    Write-Host "Native $Configuration dependencies ready. Future builds reuse $DependencyRoot."
} finally {
    if ($preparationLock) { $preparationLock.Dispose() }
}
