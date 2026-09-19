[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$BuildDirectory = 'build/debug',
    [string]$Scenario = 'inventory-only',
    [string]$SourceDescription = 'not recorded',
    [string]$StreamSettings = 'not recorded',
    [string]$NetworkConditions = 'not recorded',
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot ('build/baseline/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Use a new output directory to preserve earlier evidence: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $repoRoot $BuildDirectory
}

$collectionErrors = [Collections.Generic.List[string]]::new()
function Read-Hardware {
    param([string]$ClassName, [string[]]$Properties)
    try {
        @(Get-CimInstance -ClassName $ClassName | Select-Object -Property $Properties)
    } catch {
        $collectionErrors.Add("${ClassName}: $($_.Exception.Message)")
        return @()
    }
}
function Read-Git {
    param([string[]]$Arguments)
    $result = & git -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $Arguments failed" }
    return $result
}

$manifest = [ordered]@{
    schemaVersion = 1
    collectedAtUtc = [DateTime]::UtcNow.ToString('o')
    scenario = $Scenario
    source = $SourceDescription
    settings = $StreamSettings
    network = $NetworkConditions
    commit = Read-Git @('rev-parse', 'HEAD')
    workingTree = @(Read-Git @('status', '--short'))
    os = @(Read-Hardware 'Win32_OperatingSystem' @('Caption', 'Version', 'BuildNumber'))
    cpu = @(Read-Hardware 'Win32_Processor' @('Name', 'NumberOfCores', 'NumberOfLogicalProcessors'))
    gpu = @(Read-Hardware 'Win32_VideoController' @('Name', 'DriverVersion', 'DriverDate'))
    memory = @(Read-Hardware 'Win32_ComputerSystem' @('TotalPhysicalMemory'))
    executableSha256 = $null
    tests = [ordered]@{ status = 'not-run'; exitCode = $null }
    performance = 'not measured; inventory and test results do not establish streaming latency'
    collectionErrors = $collectionErrors
}
$executable = Join-Path $BuildDirectory 'ScreenShare.exe'
if (Test-Path -LiteralPath $executable) {
    $manifest.executableSha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
}
# Record names and hashes, never source contents, environment variables or credentials.
$changedFiles = @(Read-Git @('diff', '--name-only', 'HEAD')) + @(Read-Git @('ls-files', '--others', '--exclude-standard'))
$manifest['changedFileHashes'] = @($changedFiles | Sort-Object -Unique | ForEach-Object {
    $filePath = Join-Path $repoRoot $_
    if (Test-Path -LiteralPath $filePath -PathType Leaf) {
        [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash }
    }
})
if ($RunTests) {
    & ctest --test-dir $BuildDirectory --output-on-failure 2>&1 |
        Out-File -LiteralPath (Join-Path $OutputDirectory 'ctest.txt') -Encoding utf8
    $manifest.tests.exitCode = $LASTEXITCODE
    $manifest.tests.status = if ($LASTEXITCODE -eq 0) { 'passed' } else { 'failed' }
}
$manifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding utf8
Write-Output "Baseline evidence: $OutputDirectory"
if ($RunTests -and $manifest.tests.exitCode -ne 0) { exit $manifest.tests.exitCode }
