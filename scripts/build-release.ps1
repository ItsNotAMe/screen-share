[CmdletBinding()]
param(
    [ValidateRange(1,64)][int]$Jobs = 8,
    [string]$SigningDirectory = "$env:USERPROFILE/.screenshare-release"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/UpdateSigning.ps1"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $dirty = & git status --porcelain
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit the intended changes before building a release.' }
    $commit = (& git rev-parse HEAD).Trim()
    $version = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(ScreenShareNative VERSION (\d+\.\d+\.\d+)').Groups[1].Value
    if (-not $version) { throw 'Cannot determine the release version.' }
    $publicKey = Join-Path $SigningDirectory 'screenshare-update-public.der'
    $privateKey = Join-Path $SigningDirectory 'screenshare-update.key'
    $passphrase = Join-Path $SigningDirectory 'update-passphrase.dpapi'
    Assert-PinnedUpdatePublicKey $publicKey
    if (-not (Test-Path $passphrase)) { throw 'Run scripts/set-update-signing-secret.ps1 once under this Windows account first.' }
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $vs) { throw 'Visual Studio C++ Build Tools are required.' }
    $dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    $lines = & $env:ComSpec /d /c "call `"$dev`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the native build environment.' }
    foreach ($line in $lines) {
        if ($line -match '^(PATH|INCLUDE|LIB|LIBPATH|VC[^=]*|VS[^=]*|WindowsSdk[^=]*|WindowsSDK[^=]*|UCRT[^=]*|UniversalCRT[^=]*|ExtensionSdkDir)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process')
        }
    }
    $cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    $iscc = @("$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe", "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $iscc) { throw 'Install Inno Setup 6 before building Setup.' }
    $driver = & "$PSScriptRoot/fetch-controller-runtime.ps1"
    $sdk = ''
    if (Test-Path build/release/CMakeCache.txt) {
        $entry = Select-String -Path build/release/CMakeCache.txt -Pattern '^SCREENSHARE_WEBRTC_ARTIFACT_DIR:PATH=(.+)$'
        if ($entry) { $sdk = $entry.Matches[0].Groups[1].Value }
    }
    $sdkReady = $false
    if ($sdk -and (Test-Path (Join-Path $sdk 'screenshare-artifact.json'))) {
        $sdkReady = (Get-Content (Join-Path $sdk 'screenshare-artifact.json') -Raw | ConvertFrom-Json).schemaVersion -eq 2
    }
    if (-not $sdkReady) {
        & "$PSScriptRoot/prepare-dependencies.ps1" -Configuration Release -Jobs $Jobs
        $export = & python "$PSScriptRoot/webrtc-sdk.py" export '.deps/webrtc/checkout/src/out/screenshare-release' '.deps/webrtc-sdk-cache'
        if ($LASTEXITCODE -ne 0) { throw 'WebRTC SDK export failed.' }
        $sdk = [string]($export | Select-Object -Last 1)
    }
    & $cmake --preset release "-DSCREENSHARE_WEBRTC_ARTIFACT_DIR=$sdk" -DSCREENSHARE_BUILD_INSTALLER=ON "-DSCREENSHARE_CONTROLLER_DRIVER_SETUP=$driver" "-DSCREENSHARE_INNO_SETUP_COMPILER=$iscc"
    if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed.' }
    & $cmake --build build/release --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    & $ctest --test-dir build/release --output-on-failure --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'Release tests failed.' }
    & "$PSScriptRoot/../tests/UpdateSigningTests.ps1"
    & $cmake --build build/release --parallel $Jobs --target package-installer
    if ($LASTEXITCODE -ne 0) { throw 'Installer packaging failed.' }
    $stage = Join-Path $root 'build/release/package/ScreenShare-release-windows-x64'
    $probe = Start-Process -FilePath "$stage/ScreenShareUi.exe" -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
    if ($probe.ExitCode -ne 0) { throw 'Packaged UI self-test failed.' }
    $sources = Join-Path $root 'build/release/qt-sources'
    [IO.Directory]::CreateDirectory($sources) | Out-Null
    foreach ($line in Get-Content third_party/qt-6.10.3/SOURCE-SHA256.txt) {
        $hash, $name = $line -split '\s+',2
        $destination = Join-Path $sources $name
        if (-not (Test-Path $destination)) {
            Invoke-WebRequest -UseBasicParsing "https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/$name" -OutFile $destination
        }
        if ((Get-FileHash $destination).Hash -ne $hash) { throw "Qt source hash mismatch: $name" }
    }
    $manifest = Join-Path $root 'build/release/screenshare-update.json'
    & "$PSScriptRoot/create-update-manifest.ps1" -Version $version `
        -ZipPath build/release/ScreenShare-release-windows-x64.zip `
        -InstallerPath "build/release/ScreenShare-Setup-$version-windows-x64.exe" `
        -OutputPath $manifest -Notes @('Expanded connection, codec and performance diagnostic reports.', 'Improved internet connection discovery, decoder compatibility and isolation of stalled viewers.', 'Added the host nickname to the room list.')
    & "$PSScriptRoot/sign-update-manifest.ps1" -ManifestPath $manifest -PrivateKeyPath $privateKey -PublicKeyPath $publicKey -PassphraseFile $passphrase
    & "$PSScriptRoot/verify-update-manifest.ps1" -ManifestPath $manifest -PublicKeyPath $publicKey
    if ((& git rev-parse HEAD).Trim() -ne $commit -or (& git status --porcelain)) { throw 'The source changed during the release build.' }
    @{ version=$version; commit=$commit; builtUtc=[DateTime]::UtcNow.ToString('o') } | ConvertTo-Json |
        Set-Content build/release/release-build.json -Encoding UTF8
    Write-Host "Release $version built, tested and signed. Run scripts/publish-release.ps1 from the merged main commit to publish."
} finally { Pop-Location }
