[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content (Join-Path $repoRoot 'refactor/native-dependencies.json') -Raw | ConvertFrom-Json
function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$environmentRoot = Join-Path $repoRoot 'build/dependency-tools'
$python = Join-Path $environmentRoot 'Scripts/python.exe'
if (-not (Test-Path $python)) { Invoke-Checked python @('-m', 'venv', $environmentRoot) }
Invoke-Checked $python @('-m', 'pip', 'install', "aqtinstall==$($lock.aqtVersion)")
$qtRoot = Join-Path $repoRoot 'build/Qt'
$qtInstall = Join-Path $qtRoot "$($lock.qtVersion)/msvc2022_64"
if (-not (Test-Path (Join-Path $qtInstall 'lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake')) -or
    -not (Test-Path (Join-Path $qtInstall 'lib/cmake/Qt6Svg/Qt6SvgConfig.cmake'))) {
    Invoke-Checked $python (@('-m', 'aqt', 'install-qt', 'windows', 'desktop', $lock.qtVersion,
        $lock.qtArchitecture, '-O', $qtRoot, '-m') + @($lock.qtModules))
}
$headers = Join-Path $repoRoot 'build/Vulkan-Headers'
if (-not (Test-Path $headers)) {
    Invoke-Checked git @('clone', '--depth', '1', $lock.vulkanHeadersRepository, $headers)
}
$changes = & git -C $headers status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Vulkan-Headers must be a clean Git checkout' }
Invoke-Checked git @('-C', $headers, 'fetch', '--depth', '1', 'origin', $lock.vulkanHeadersCommit)
Invoke-Checked git @('-C', $headers, 'checkout', '--detach', $lock.vulkanHeadersCommit)
Write-Output "Native Qt and Vulkan headers ready. Windows SDK requirement: $($lock.windowsSdkPackageVersion)."
