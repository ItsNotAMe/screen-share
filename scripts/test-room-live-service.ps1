param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Origin,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
# Silent synthetic endpoints: no speaker output, screen capture or OS input.
$ErrorActionPreference = 'Stop'
$uri = [Uri]$Origin
if (-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or $uri.UserInfo -or
    $uri.AbsolutePath -ne '/' -or $uri.Query -or $uri.Fragment) {
    throw 'Supply an HTTPS service origin without credentials, path, query or fragment.'
}
$binary = (Resolve-Path -LiteralPath $Executable).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new evidence directory; existing evidence is preserved.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$report = [ordered]@{
    schema = 1; scenario = 'live-service-public-session'; passed = $false
    origin = $uri.GetLeftPart([UriPartial]::Authority)
    startedUtc = [DateTime]::UtcNow.ToString('o')
    executableSha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    runnerSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    machine = $env:COMPUTERNAME; mediaScope = 'four peers on this machine; remote HTTPS/WSS signaling'
    physicalInput = $false; audibleOutput = $false; timedOut = $false; exitCode = $null
}
$timer = [Diagnostics.Stopwatch]::StartNew()
$process = New-Object Diagnostics.Process
$process.StartInfo.FileName = $binary
$process.StartInfo.Arguments = $report.origin
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
    if (-not $process.WaitForExit(120000)) {
        $report.timedOut = $true
        $process.Kill()
        $process.WaitForExit()
    }
    $report.exitCode = $process.ExitCode
    $stdout = $outTask.GetAwaiter().GetResult()
    $stderr = $errTask.GetAwaiter().GetResult()
    if ($report.timedOut -or $report.exitCode -ne 0) { throw 'Native proof failed or exceeded its deadline.' }
    $metrics = $stdout | ConvertFrom-Json
    foreach ($name in @('passed', 'public_session', 'native_runtime', 'rejoin', 'authorized_input',
        'cancel_admission', 'coalesced_stop', 'media_drain_barrier', 'production_tls_required')) {
        if ($metrics.$name -isnot [bool] -or $metrics.$name -ne $true) { throw "Missing/failed assertion: $name" }
    }
    if ($metrics.diagnostic_plaintext -isnot [bool] -or $metrics.diagnostic_plaintext -ne $false -or
        $metrics.viewers -ne 4 -or $metrics.decoded_frames -lt 180) {
        throw 'Missing TLS policy or four-viewer media evidence.'
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
