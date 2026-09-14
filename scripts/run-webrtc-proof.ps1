[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')][string]$Configuration = 'debug',
    [switch]$Application,
    [switch]$Hardware,
    [switch]$AudioDevice,
    [switch]$LiveCapture,
    [string]$ArtifactDirectory,
    [string]$BuildDirectory,
    [string]$ViGEmSourceDirectory,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Value) {
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Value))
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC x64 build tools are required.' }
$cmakeBin = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'
if (-not (Test-Path $cmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
$developerCommand = Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
if ($developerCommand.Contains('"')) { throw 'Unexpected quote in Visual Studio installation path' }
# Capture only the developer environment into this process; never print it.
$environmentLines = & $env:ComSpec /d /c "call `"$developerCommand`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize Visual Studio environment' }
$saved = @{}
try {
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2]
            # Only build-tool variables are relevant. Leave profile/system identity alone.
            if ($name -match '^(PATH|INCLUDE|LIB|LIBPATH|VC.*|VS.*|WindowsSdk.*|WindowsSDK.*|UCRT.*|UniversalCRT.*|ExtensionSdkDir)$') {
                $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
                [Environment]::SetEnvironmentVariable($name, $value, 'Process')
            }
        }
    }
    $projectDirectory = if ($Application) { $repoRoot } else { Join-Path $repoRoot 'tools/webrtc-proof' }
    $preset = if ($Application) { "native-$Configuration" } else { $Configuration }
    Push-Location $projectDirectory
    try {
        $configureArguments = @('--preset', $preset)
        if ($ArtifactDirectory) { $configureArguments += "-DSCREENSHARE_WEBRTC_ARTIFACT_DIR=$(Resolve-RepoPath $ArtifactDirectory)" }
        if ($ViGEmSourceDirectory) { $configureArguments += "-DSCREENSHARE_VIGEMCLIENT_SOURCE_DIR=$(Resolve-RepoPath $ViGEmSourceDirectory)" }
        if ($BuildDirectory) {
            $BuildDirectory = Resolve-RepoPath $BuildDirectory
            $configureArguments += @('-B', $BuildDirectory)
        }
        if (-not $Application) {
            $configureArguments += @("-DSCREENSHARE_TEST_HARDWARE=$($Hardware.IsPresent.ToString().ToUpperInvariant())", "-DSCREENSHARE_TEST_AUDIO_DEVICE=$($AudioDevice.IsPresent.ToString().ToUpperInvariant())", "-DSCREENSHARE_TEST_LIVE_CAPTURE=$($LiveCapture.IsPresent.ToString().ToUpperInvariant())")
        }
        & $cmake @configureArguments
        if ($LASTEXITCODE -ne 0) { throw 'Proof configuration failed' }
        if ($BuildDirectory) { & $cmake --build $BuildDirectory --parallel 8 }
        else { & $cmake --build --preset $preset }
        if ($LASTEXITCODE -ne 0) { throw 'Proof build failed' }
        if ($BuildDirectory) { & $ctest --test-dir $BuildDirectory --output-on-failure }
        elseif ($Application) { & $ctest --test-dir "build/native-$Configuration" --output-on-failure }
        else { & $ctest --preset $Configuration }
        if ($LASTEXITCODE -ne 0) { throw 'Proof test failed' }
        if ($Package) {
            if (-not $Application) { throw '-Package requires -Application' }
            if ($BuildDirectory) { & $cmake --build $BuildDirectory --target package-portable }
            else { & $cmake --build --preset $preset --target package-portable }
            if ($LASTEXITCODE -ne 0) { throw 'Native packaging failed' }
        }
    } finally { Pop-Location }
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
