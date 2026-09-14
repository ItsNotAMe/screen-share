[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$DependencyRoot,
    [ValidateRange(1, 64)][int]$Jobs = 8,
    [switch]$GenerateOnly,
    [switch]$SkipHooks
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $DependencyRoot) { $DependencyRoot = Join-Path $repoRoot 'build/webrtc' }
$DependencyRoot = [IO.Path]::GetFullPath($DependencyRoot)
$sourceRoot = Join-Path $DependencyRoot 'checkout/src'
$depotRoot = Join-Path $DependencyRoot 'depot_tools'
$lock = Get-Content (Join-Path $repoRoot 'refactor/webrtc-source.json') -Raw | ConvertFrom-Json
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
foreach ($entry in @(@($sourceRoot, $lock.commit), @((Join-Path $sourceRoot 'build'), $lock.chromiumBuildCommit), @($depotRoot, $lock.depotToolsCommit))) {
    $actual = & git -C $entry[0] rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -ne $entry[1]) { throw "Source revision mismatch: $($entry[0]). Run sync-webrtc.ps1." }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Install Visual Studio C++ x64 build tools first.' }
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
if (-not (Test-Path (Join-Path $sdkRoot 'Lib/10.0.28000.0/um/x64/kernel32.lib'))) {
    throw 'Pinned Chromium build requires Windows SDK 10.0.28000.0. Install Microsoft.WindowsSDK.10.0.28000.'
}
$patch = Join-Path $repoRoot 'refactor/webrtc-build.patch'
$buildRoot = Join-Path $sourceRoot 'build'
& git -C $buildRoot apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -ne 0) {
    Invoke-Checked git @('-C', $buildRoot, 'apply', '--check', $patch)
    Invoke-Checked git @('-C', $buildRoot, 'apply', $patch)
}
$savedEnvironment = @{}
foreach ($name in @('PATH', 'DEPOT_TOOLS_UPDATE', 'DEPOT_TOOLS_WIN_TOOLCHAIN', 'GYP_MSVS_OVERRIDE_PATH')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:PATH = "$depotRoot;$env:PATH"
    $env:DEPOT_TOOLS_UPDATE = '0'
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
    $env:GYP_MSVS_OVERRIDE_PATH = $vsRoot
    if (-not $SkipHooks) {
        Push-Location (Split-Path -Parent $sourceRoot)
        try { Invoke-Checked (Join-Path $depotRoot 'gclient.bat') @('runhooks') } finally { Pop-Location }
    }
    $output = Join-Path $sourceRoot "out/screenshare-$($Configuration.ToLowerInvariant())"
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $debugValue = if ($Configuration -eq 'Debug') { 'true' } else { 'false' }
    $arguments = @(
        'target_os="win"', 'target_cpu="x64"', 'is_clang=true', "is_debug=$debugValue",
        'is_component_build=false', 'use_custom_libcxx=false', 'use_custom_libcxx_for_host=false',
        'use_rtti=true', 'use_cxx23=false', 'enable_iterator_debugging=true', 'rtc_include_tests=true', 'rtc_build_examples=false',
        'rtc_enable_sctp=true', 'rtc_use_h264=true', 'proprietary_codecs=true', 'ffmpeg_branding="Chrome"',
        'use_remoteexec=false', 'use_siso=false'
    )
    $arguments -join "`n" | Set-Content (Join-Path $output 'args.gn') -Encoding ascii
    Push-Location $sourceRoot
    try {
        Invoke-Checked (Join-Path $depotRoot 'gn.bat') @('gen', $output, '--fail-on-unused-args')
        if (-not $GenerateOnly) {
            Invoke-Checked (Join-Path $depotRoot 'ninja.bat') @('-C', $output, '-j', "$Jobs", 'webrtc', 'webrtc_lib_link_test')
            Invoke-Checked (Join-Path $output 'webrtc_lib_link_test.exe') @()
            $gn = Join-Path $depotRoot 'gn.bat'
            $definesJson = & $gn desc $output '//:webrtc' defines --format=json
            if ($LASTEXITCODE -ne 0) { throw 'Could not export WebRTC defines' }
            $librariesJson = & $gn desc $output '//:webrtc_lib_link_test' libs --format=json
            if ($LASTEXITCODE -ne 0) { throw 'Could not export WebRTC system libraries' }
            $compiler = Join-Path $sourceRoot 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe'
            $runtime = if ($Configuration -eq 'Debug') { 'MultiThreadedDebugDLL' } else { 'MultiThreadedDLL' }
            $metadata = [ordered]@{
                schemaVersion = 1
                sourceRevision = $lock.commit
                configuration = $Configuration
                architecture = 'x64'
                msvcToolset = (Get-Content (Join-Path $vsRoot 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
                windowsSdk = '10.0.28000.0'
                runtime = $runtime
                standardLibrary = 'msvc'
                sourceDirectory = $sourceRoot.Replace('\', '/')
                compilerSha256 = (Get-FileHash $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
                librarySha256 = (Get-FileHash (Join-Path $output 'obj/webrtc.lib') -Algorithm SHA256).Hash.ToLowerInvariant()
                gnArgumentsSha256 = (Get-FileHash (Join-Path $output 'args.gn') -Algorithm SHA256).Hash.ToLowerInvariant()
                buildPatchSha256 = (Get-FileHash $patch -Algorithm SHA256).Hash.ToLowerInvariant()
                defines = @(($definesJson | ConvertFrom-Json).'//:webrtc'.defines | Where-Object { $_ -ne 'WEBRTC_LIBRARY_IMPL' })
                systemLibraries = @(($librariesJson | ConvertFrom-Json).'//:webrtc_lib_link_test'.libs | ForEach-Object {
                    if ($_.StartsWith('//')) { Join-Path $sourceRoot $_.Substring(2) } else { $_ }
                })
            }
            $metadata | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $output 'screenshare-artifact.json') -Encoding utf8
        }
    } finally { Pop-Location }
    Write-Output "WebRTC $Configuration output: $output"
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
}
