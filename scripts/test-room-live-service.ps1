param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Origin,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('public-session', 'cli', 'cross-host', 'cross-viewer', 'load-host', 'load-viewer')][string]$Scenario = 'public-session',
    [string]$RoomId,
    [string]$ReadyFile,
    [ValidateRange(10,300)][int]$Seconds = 60,
    [ValidateSet('hardware','software')][string]$Decoder = 'hardware'
)
# Silent endpoints; load-host captures only its own generated window. No OS input.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'RoomLiveEvidence.ps1')
$uri = [Uri]$Origin
if (-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or $uri.UserInfo -or
    $uri.AbsolutePath -ne '/' -or $uri.Query -or $uri.Fragment) {
    throw 'Supply an HTTPS service origin without credentials, path, query or fragment.'
}
$binary = (Resolve-Path -LiteralPath $Executable).Path
if ($Scenario -in @('cross-viewer','load-viewer') -and $RoomId -notmatch '^[A-Za-z0-9_-]{1,128}$') { throw 'Supply a valid room ID.' }
if ($Scenario -in @('cross-host','load-host')) {
    if (-not $ReadyFile -or $ReadyFile.Contains('"')) { throw 'Supply a readiness file path without quotes.' }
    $ReadyFile = [IO.Path]::GetFullPath($ReadyFile)
    if (Test-Path -LiteralPath $ReadyFile) { throw 'Readiness file already exists.' }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory; existing evidence is preserved.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$report = [ordered]@{
    schema = 1; scenario = "live-service-$Scenario"; passed = $false
    origin = $uri.GetLeftPart([UriPartial]::Authority)
    startedUtc = [DateTime]::UtcNow.ToString('o')
    executableSha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    runnerSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    validatorSha256 = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'RoomLiveEvidence.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
    machine = $env:COMPUTERNAME; mediaScope = 'same-machine media peers; remote HTTPS/WSS signaling'
    physicalInput = $false; audibleOutput = $false; timedOut = $false; exitCode = $null
}
$timer = [Diagnostics.Stopwatch]::StartNew()
$process = New-Object Diagnostics.Process
$process.StartInfo.FileName = $binary
$process.StartInfo.Arguments = $report.origin
if ($Scenario -eq 'cross-host') { $process.StartInfo.Arguments = 'host ' + $report.origin + ' "' + $ReadyFile + '"' }
if ($Scenario -eq 'cross-viewer') { $process.StartInfo.Arguments = 'viewer ' + $report.origin + ' ' + $RoomId }
if ($Scenario.StartsWith('cross-')) { $report.mediaScope = 'cross-machine scenario; endpoint placement recorded by launcher' }
if ($Scenario.StartsWith('load-')) {
    $report.mediaScope = 'generated 1080p WGC window to remote CPU pixel consumer; no physical latency'
    $report.requestedSeconds = $Seconds
    $report.decoderMode = $Decoder
    $argument = if ($Scenario -eq 'load-host') { '"' + $ReadyFile + '"' } else { $RoomId }
    $process.StartInfo.Arguments = $Scenario + ' ' + $report.origin + ' ' + $argument + ' ' + $Seconds + ' ' + $Decoder
}
$process.StartInfo.WorkingDirectory = Split-Path -Parent $binary
$process.StartInfo.UseShellExecute = $false
$process.StartInfo.CreateNoWindow = $true
$process.StartInfo.RedirectStandardOutput = $true
$process.StartInfo.RedirectStandardError = $true
$stdout = ''; $stderr = ''
try {
    if (-not $process.Start()) { throw 'Unable to start proof.' }
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    $deadlineMs = if ($Scenario.StartsWith('load-')) { ($Seconds + 110) * 1000 } else { 120000 }
    if (-not $process.WaitForExit($deadlineMs)) {
        $report.timedOut = $true
        $process.Kill()
        $process.WaitForExit()
    }
    $report.exitCode = $process.ExitCode
    $stdout = $outTask.GetAwaiter().GetResult()
    $stderr = $errTask.GetAwaiter().GetResult()
    if ($report.timedOut -or $report.exitCode -ne 0) { throw 'Native proof failed or exceeded its deadline.' }
    $metrics = $stdout | ConvertFrom-Json
    $assertions = if ($Scenario -eq 'cli') {
        @('passed', 'cli_session', 'command_options', 'live_settings', 'bounded_presentation', 'silent_audio')
    } elseif ($Scenario.StartsWith('load-')) {
        @('passed', 'runtimeReleased')
    } elseif ($Scenario.StartsWith('cross-')) {
        @('passed', 'freshRejoin', 'runtimeReleased', 'productionTls')
    } else {
        @('passed', 'public_session', 'native_runtime', 'rejoin', 'authorized_input',
            'cancel_admission', 'coalesced_stop', 'media_drain_barrier', 'production_tls_required')
    }
    foreach ($name in $assertions) {
        if ($metrics.$name -isnot [bool] -or $metrics.$name -ne $true) { throw "Missing/failed assertion: $name" }
    }
    if ($Scenario.StartsWith('load-')) { Assert-RoomLoadEvidence $metrics $Scenario.Substring(5) $Seconds $Decoder }
    if ($Scenario -eq 'public-session' -and
        ($metrics.diagnostic_plaintext -isnot [bool] -or $metrics.diagnostic_plaintext -ne $false -or
        $metrics.viewers -ne 4 -or $metrics.decoded_frames -lt 180)) {
        throw 'Missing TLS policy or four-viewer media evidence.'
    }
    if ($Scenario -eq 'cli' -and ($metrics.original_frames -lt 10 -or $metrics.changed_frames -lt 10)) {
        throw 'Missing CLI before/after media progress.'
    }
    if ($Scenario.StartsWith('cross-')) {
        if ($metrics.physicalInput -isnot [bool] -or $metrics.physicalInput -ne $false -or
            $metrics.audibleOutput -isnot [bool] -or $metrics.audibleOutput -ne $false -or
            $metrics.role -ne $Scenario.Substring(6)) { throw 'Unexpected endpoint role or physical effects.' }
        if ($Scenario -eq 'cross-host' -and ($metrics.inputEvents -lt 40 -or $metrics.fixedAndAutoSettings -ne $true)) {
            throw 'Missing host input/settings evidence.'
        }
        if ($Scenario -eq 'cross-viewer') {
            if ($metrics.restart -ne $true -or $metrics.frames -lt 180 -or $metrics.audioBlocks -lt 40 -or
                $metrics.inputImageSamplesMs.Count -ne 20 -or $metrics.externalLatencyVerified -ne $false) {
                throw 'Missing viewer progress/restart/internal timing evidence.'
            }
            Assert-RoomTimingSamples $metrics.inputImageSamplesMs
        }
    }
    $report.metrics = $metrics
    $report.passed = $true
} catch {
    $report.error = $_.Exception.Message
} finally {
    $report.elapsedMs = $timer.ElapsedMilliseconds
    $process.Dispose()
    [IO.File]::WriteAllText((Join-Path $output 'stdout.log'), $stdout)
    [IO.File]::WriteAllText((Join-Path $output 'stderr.log'), $stderr)
    $report.stdoutSha256 = (Get-FileHash -LiteralPath (Join-Path $output 'stdout.log')).Hash.ToLowerInvariant()
    $report.stderrSha256 = (Get-FileHash -LiteralPath (Join-Path $output 'stderr.log')).Hash.ToLowerInvariant()
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
}
$report | ConvertTo-Json -Depth 8
if (-not $report.passed) { exit 1 }
