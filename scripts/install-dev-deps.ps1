#Requires -Version 5.1

<#
.SYNOPSIS
Installs the development dependencies used by ScreenShare on Windows.

.DESCRIPTION
This script bootstraps the native MSYS2/MinGW toolchain and the optional
Cloudflare Worker TypeScript tooling. It is intentionally explicit: use
-DryRun first if you want to see the commands before they run.

Examples:
  .\scripts\install-dev-deps.ps1
  .\scripts\install-dev-deps.ps1 -DryRun
  .\scripts\install-dev-deps.ps1 -SkipQt -SkipFfmpeg
  .\scripts\install-dev-deps.ps1 -WorkerOnly
#>

[CmdletBinding()]
param(
    [switch]$NativeOnly,
    [switch]$WorkerOnly,
    [switch]$SkipNative,
    [switch]$SkipWorker,
    [switch]$SkipQt,
    [switch]$SkipFfmpeg,
    [switch]$InstallWindowsSdk,
    [switch]$UpdateUserPath,
    [switch]$ConfigureDebug,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$MsysRoot = "C:\msys64"
$MsysBash = Join-Path $MsysRoot "usr\bin\bash.exe"
$UcrtBin = Join-Path $MsysRoot "ucrt64\bin"
$MsysUsrBin = Join-Path $MsysRoot "usr\bin"
$NativePathTouched = $false
$InstalledNode = $false

if ($NativeOnly -and $WorkerOnly) {
    throw "Use only one of -NativeOnly or -WorkerOnly."
}

if ($NativeOnly) {
    $SkipWorker = $true
}
if ($WorkerOnly) {
    $SkipNative = $true
}

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Info {
    param([string]$Message)
    Write-Host "    $Message"
}

function Test-Command {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @()
    )

    $display = "$FilePath $($Arguments -join ' ')".Trim()
    if ($DryRun) {
        Write-Host "DRY RUN: $display" -ForegroundColor DarkYellow
        return
    }

    Write-Host $display -ForegroundColor DarkGray
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $display"
    }
}

function Invoke-WingetInstall {
    param(
        [Parameter(Mandatory = $true)][string]$PackageId,
        [string[]]$ExtraArguments = @()
    )

    if (-not $DryRun -and -not (Test-Command "winget")) {
        throw "winget was not found. Install App Installer from Microsoft Store, then rerun this script."
    }

    $arguments = @(
        "install",
        "--id", $PackageId,
        "-e",
        "--accept-package-agreements",
        "--accept-source-agreements"
    ) + $ExtraArguments

    Invoke-Tool "winget" $arguments
}

function Add-ProcessPath {
    param([string]$PathToAdd)
    if (-not (Test-Path $PathToAdd)) {
        return
    }
    $parts = $env:Path -split ";"
    if ($parts -notcontains $PathToAdd) {
        $env:Path = "$PathToAdd;$env:Path"
    }
}

function Add-UserPath {
    param([string]$PathToAdd)
    if (-not (Test-Path $PathToAdd)) {
        return
    }

    $current = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = @()
    if (-not [string]::IsNullOrWhiteSpace($current)) {
        $parts = $current -split ";"
    }
    if ($parts -contains $PathToAdd) {
        return
    }

    $next = if ([string]::IsNullOrWhiteSpace($current)) {
        $PathToAdd
    } else {
        "$current;$PathToAdd"
    }

    if ($DryRun) {
        Write-Host "DRY RUN: update user PATH with $PathToAdd" -ForegroundColor DarkYellow
        return
    }
    [Environment]::SetEnvironmentVariable("Path", $next, "User")
}

function Ensure-Msys2 {
    if (Test-Path $MsysBash) {
        Write-Info "MSYS2 found at $MsysRoot"
        return
    }

    Write-Step "Installing MSYS2"
    Invoke-WingetInstall "MSYS2.MSYS2"
    if (-not $DryRun -and -not (Test-Path $MsysBash)) {
        throw "MSYS2 install finished, but $MsysBash was not found. Open a new terminal and rerun the script."
    }
}

function Invoke-Msys2 {
    param([Parameter(Mandatory = $true)][string]$Command)
    Invoke-Tool $MsysBash @("-lc", $Command)
}

function Install-MsysPackages {
    $packages = @(
        "mingw-w64-ucrt-x86_64-toolchain",
        "mingw-w64-ucrt-x86_64-cmake",
        "mingw-w64-ucrt-x86_64-make",
        "mingw-w64-ucrt-x86_64-pkgconf",
        "mingw-w64-ucrt-x86_64-opus"
    )

    if (-not $SkipQt) {
        $packages += "mingw-w64-ucrt-x86_64-qt6-base"
        $packages += "mingw-w64-ucrt-x86_64-qt6-svg"
    }
    if (-not $SkipFfmpeg) {
        $packages += "mingw-w64-ucrt-x86_64-ffmpeg"
    }

    Write-Step "Updating MSYS2 package database"
    Invoke-Msys2 "pacman --noconfirm -Syu"

    Write-Step "Installing native build packages"
    $packageLine = $packages -join " "
    Invoke-Msys2 "pacman --noconfirm --needed -S $packageLine"
}

function Test-CppWinRtHeaders {
    $headers = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include" `
        -Directory `
        -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "cppwinrt\winrt\Windows.Graphics.Capture.h" } |
        Where-Object { Test-Path $_ }
    return $null -ne $headers
}

function Ensure-WindowsSdk {
    if (Test-CppWinRtHeaders) {
        Write-Info "Windows SDK C++/WinRT headers found"
        return
    }

    if (-not $InstallWindowsSdk) {
        Write-Warning "Windows SDK C++/WinRT headers were not found. If CMake later fails on winrt headers, rerun with -InstallWindowsSdk or install Visual Studio Build Tools with a Windows 10/11 SDK."
        return
    }

    Write-Step "Installing Visual Studio Build Tools and Windows SDK"
    Invoke-WingetInstall "Microsoft.VisualStudio.2022.BuildTools" @(
        "--override",
        "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
    )
}

function Ensure-Node {
    if (Test-Command "node" -and Test-Command "npm") {
        Write-Info "Node.js and npm found"
        return
    }

    Write-Step "Installing Node.js LTS"
    Invoke-WingetInstall "OpenJS.NodeJS.LTS"
    Add-ProcessPath "C:\Program Files\nodejs"
    $script:InstalledNode = $true
}

function Install-WorkerPackages {
    $workerDir = Join-Path $RepoRoot "signaling-worker"
    if (-not (Test-Path (Join-Path $workerDir "package.json"))) {
        Write-Info "No signaling-worker/package.json found; skipping Worker npm install"
        return
    }

    Write-Step "Installing signaling Worker npm dependencies"
    Push-Location $workerDir
    try {
        Invoke-Tool "npm" @("install")
    } finally {
        Pop-Location
    }
}

function Configure-DebugBuild {
    Write-Step "Configuring debug CMake preset"
    Push-Location $RepoRoot
    try {
        Invoke-Tool "cmake" @("--preset", "debug")
    } finally {
        Pop-Location
    }
}

Write-Step "ScreenShare dependency bootstrap"
Write-Info "Repo: $RepoRoot"
if ($DryRun) {
    Write-Info "Dry run enabled. No installs or PATH changes will be performed."
}

if (-not $SkipNative) {
    Ensure-Msys2
    Add-ProcessPath $UcrtBin
    Add-ProcessPath $MsysUsrBin
    $NativePathTouched = $true
    if ($UpdateUserPath) {
        Add-UserPath $UcrtBin
        Add-UserPath $MsysUsrBin
    }
    Install-MsysPackages
    Ensure-WindowsSdk
} else {
    Write-Info "Skipping native C++ dependencies."
}

if (-not $SkipWorker) {
    Ensure-Node
    Install-WorkerPackages
} else {
    Write-Info "Skipping signaling Worker dependencies."
}

if ($ConfigureDebug) {
    Add-ProcessPath $UcrtBin
    Configure-DebugBuild
}

Write-Step "Done"
if ($NativePathTouched) {
    Write-Info "For this terminal, MSYS2 paths were prepended when available."
}
if ($NativePathTouched -and -not $UpdateUserPath) {
    Write-Info "To persist MSYS2 paths for future terminals, rerun with -UpdateUserPath."
}
if (-not $SkipNative) {
    Write-Info "Next build commands:"
    Write-Info "  cmake --preset debug"
    Write-Info "  cmake --build --preset debug"
}
if (-not $SkipWorker) {
    Write-Info "Worker commands:"
    Write-Info "  cd signaling-worker"
    Write-Info "  npm run typecheck"
    Write-Info "  npx wrangler deploy"
}
if ($NativePathTouched -or $InstalledNode) {
    Write-Info "Open a new terminal after installation if node/npm/cmake are not visible in your current shell."
}
