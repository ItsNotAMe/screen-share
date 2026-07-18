[CmdletBinding()]
param(
    [ValidateSet('Local', 'Host', 'Viewer', 'Summarize')]
    [string]$Role = 'Local',

    [ValidateSet('Healthy', 'LossJitter', 'SlowConsumer', 'LateJoin', 'LeaveRejoin', 'Unreachable')]
    [string]$Scenario = 'Healthy',

    [string]$RoomId,
    [string]$SignalServer,
    [string]$HostAddress,
    [int]$ViewerIndex = 1,
    [ValidateRange(2, 3)]
    [int]$ViewerCount = 2,
    [ValidateRange(30, 7200)]
    [int]$DurationSeconds = 600,
    [ValidateRange(1024, 65000)]
    [int]$BasePort = 55100,
    [string]$OutputRoot,
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $repoRoot 'build\release\ScreenShare.exe'
}
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Release executable not found: $Executable. Build the release preset first."
}

$runStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($RoomId)) {
    $RoomId = "mv-$runStamp"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot "build\multiviewer\$RoomId-$Scenario"
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

function Add-SignalingArguments {
    param([System.Collections.Generic.List[string]]$Arguments)
    $Arguments.Add('--signal-room')
    $Arguments.Add($RoomId)
    if (-not [string]::IsNullOrWhiteSpace($SignalServer)) {
        $Arguments.Add('--signal-server')
        $Arguments.Add($SignalServer)
    }
}

function New-ViewerArguments {
    param([int]$Index, [int]$Seconds, [string]$SessionSuffix = '')
    $port = $BasePort + $Index
    $args = [System.Collections.Generic.List[string]]::new()
    $args.Add('--watch')
    $args.Add([string]$port)
    $args.Add('--seconds')
    $args.Add([string]$Seconds)
    $args.Add('--audio-playback-muted')
    $args.Add('--session')
    $args.Add("$RoomId-viewer-$Index$SessionSuffix")
    $args.Add('--save-report')
    $args.Add((Join-Path $OutputRoot "viewer-$Index-report.zip"))
    Add-SignalingArguments $args

    if ($Index -eq $ViewerCount -and $Scenario -eq 'LossJitter') {
        $args.Add('--simulate-loss-percent')
        $args.Add('5')
        $args.Add('--simulate-jitter-ms')
        $args.Add('200')
    }
    if ($Index -eq $ViewerCount -and $Scenario -eq 'SlowConsumer') {
        $args.Add('--simulate-receive-delay-ms')
        $args.Add('2')
    }
    return $args
}

function New-HostArguments {
    $args = [System.Collections.Generic.List[string]]::new()
    $args.Add('--share-room')
    $args.Add([string]$BasePort)
    $args.Add('--width')
    $args.Add('1920')
    $args.Add('--height')
    $args.Add('1080')
    $args.Add('--fps')
    $args.Add('60')
    $args.Add('--seconds')
    $args.Add([string]$DurationSeconds)
    $args.Add('--session')
    $args.Add("$RoomId-host")
    $args.Add('--save-report')
    $args.Add((Join-Path $OutputRoot 'host-report.zip'))
    Add-SignalingArguments $args
    return $args
}

function Start-HarnessProcess {
    param([string]$Name, [System.Collections.Generic.List[string]]$Arguments)
    $stdout = Join-Path $OutputRoot "$Name.stdout.log"
    $stderr = Join-Path $OutputRoot "$Name.stderr.log"
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments.ToArray() -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    return [pscustomobject]@{ Name = $Name; Process = $process; Arguments = $Arguments; Stdout = $stdout; Stderr = $stderr }
}

function Write-Manifest {
    param([array]$Processes)
    $manifest = [ordered]@{
        runId = $RoomId
        role = $Role
        scenario = $Scenario
        viewerCount = $ViewerCount
        durationSeconds = $DurationSeconds
        basePort = $BasePort
        startedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
        executable = $Executable
        processes = @($Processes | ForEach-Object {
            [ordered]@{ name = $_.Name; arguments = @($_.Arguments); stdout = $_.Stdout; stderr = $_.Stderr }
        })
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputRoot 'manifest.json') -Encoding utf8
}

function Write-Summary {
    $rows = [System.Collections.Generic.List[object]]::new()
    Get-ChildItem -LiteralPath $OutputRoot -Filter '*.stdout.log' | ForEach-Object {
        $lastViewer = Select-String -LiteralPath $_.FullName -Pattern 'viewer_id=|viewer_target=' | Select-Object -Last 1
        $lastStats = Select-String -LiteralPath $_.FullName -Pattern 'completed_frames=|stream encoded frames:' | Select-Object -Last 1
        $rows.Add([pscustomobject]@{
            process = $_.BaseName.Replace('.stdout', '')
            viewer = if ($null -eq $lastViewer) { '' } else { $lastViewer.Line }
            finalStats = if ($null -eq $lastStats) { '' } else { $lastStats.Line }
        })
    }
    $rows | Export-Csv -LiteralPath (Join-Path $OutputRoot 'summary.csv') -NoTypeInformation -Encoding utf8

    $latestByViewer = @{}
    $hostLog = Join-Path $OutputRoot 'host.stdout.log'
    if (Test-Path -LiteralPath $hostLog -PathType Leaf) {
        Get-Content -LiteralPath $hostLog | ForEach-Object {
            $fields = @{}
            foreach ($match in [regex]::Matches($_, '(?<key>[a-z_]+)=(?<value>\S+)')) {
                $fields[$match.Groups['key'].Value] = $match.Groups['value'].Value
            }
            if ($_ -like '*signaling_live_sender_peer=removed*' -and $fields.ContainsKey('peer_id')) {
                [void]$latestByViewer.Remove($fields['peer_id'])
            } elseif ($_ -like '*viewer_target=*' -and $fields.ContainsKey('viewer_id')) {
                foreach ($staleId in @($latestByViewer.Keys)) {
                    if ($staleId -ne $fields['viewer_id'] -and
                        $latestByViewer[$staleId].endpoint -eq $fields['viewer_endpoint']) {
                        [void]$latestByViewer.Remove($staleId)
                    }
                }
                $latestByViewer[$fields['viewer_id']] = [pscustomobject]@{
                    viewerId = $fields['viewer_id']
                    state = $fields['viewer_state']
                    endpoint = $fields['viewer_endpoint']
                    completedFrames = [uint64]$fields['viewer_feedback_completed_frames']
                    queueMs = [uint64]$fields['viewer_queue_ms']
                    drops = [uint64]$fields['viewer_drops']
                    socketErrors = [uint64]$fields['viewer_socket_errors']
                    feedbackAgeMs = [uint64]$fields['viewer_feedback_age_ms']
                    joinToFirstFrameMs = [uint64]$fields['viewer_join_to_first_frame_ms']
                }
            }
        }
    }
    $viewerRows = @($latestByViewer.Values | Sort-Object viewerId)
    $viewerRows | Export-Csv -LiteralPath (Join-Path $OutputRoot 'viewer-summary.csv') -NoTypeInformation -Encoding utf8

    $expectedViewers = if ($Role -in @('Local', 'Host')) { $ViewerCount } else { 0 }
    $failures = [System.Collections.Generic.List[string]]::new()
    if ($expectedViewers -gt 0 -and $viewerRows.Count -ne $expectedViewers) {
        $failures.Add("expected $expectedViewers viewers, observed $($viewerRows.Count)")
    }
    foreach ($viewer in $viewerRows) {
        if ($viewer.completedFrames -eq 0) {
            $failures.Add("$($viewer.viewerId) completed no frames")
        }
        if ($viewer.socketErrors -ne 0) {
            $failures.Add("$($viewer.viewerId) reported $($viewer.socketErrors) socket errors")
        }
        if ($Scenario -eq 'Healthy' -and $viewer.drops -ne 0) {
            $failures.Add("$($viewer.viewerId) dropped $($viewer.drops) datagrams in the healthy scenario")
        }
    }
    $script:SummaryPassed = $failures.Count -eq 0
    [ordered]@{
        passed = $script:SummaryPassed
        expectedViewers = $expectedViewers
        observedViewers = $viewerRows.Count
        failures = @($failures)
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'assertions.json') -Encoding utf8
    Write-Output "Harness artifacts: $OutputRoot"
}

if ($Role -eq 'Summarize') {
    Write-Summary
    exit $(if ($script:SummaryPassed) { 0 } else { 1 })
}

if ($Role -eq 'Host') {
    $hostRun = Start-HarnessProcess 'host' (New-HostArguments)
    Write-Manifest @($hostRun)
    $hostRun.Process.WaitForExit()
    Write-Summary
    exit $(if ($hostRun.Process.ExitCode -eq 0 -and $script:SummaryPassed) { 0 } else { 1 })
}

if ($Role -eq 'Viewer') {
    $viewerRun = Start-HarnessProcess "viewer-$ViewerIndex" (New-ViewerArguments $ViewerIndex $DurationSeconds)
    Write-Manifest @($viewerRun)
    $viewerRun.Process.WaitForExit()
    Write-Summary
    exit $viewerRun.Process.ExitCode
}

$runs = [System.Collections.Generic.List[object]]::new()
for ($index = 1; $index -le $ViewerCount; ++$index) {
    if ($Scenario -eq 'LateJoin' -and $index -eq $ViewerCount) {
        continue
    }
    # Viewers start first; the two-second allowance keeps their deadline aligned
    # with the host instead of producing expected token errors after room close.
    $runs.Add((Start-HarnessProcess "viewer-$index" (New-ViewerArguments $index ($DurationSeconds + 2))))
}
Start-Sleep -Seconds 2
$runs.Add((Start-HarnessProcess 'host' (New-HostArguments)))

if ($Scenario -eq 'LateJoin') {
    Start-Sleep -Seconds 10
    $runs.Add((Start-HarnessProcess "viewer-$ViewerCount" (New-ViewerArguments $ViewerCount ($DurationSeconds - 10))))
}
if ($Scenario -eq 'Unreachable') {
    Start-Sleep -Seconds 15
    $victim = $runs | Where-Object Name -eq "viewer-$ViewerCount" | Select-Object -First 1
    if ($null -ne $victim -and -not $victim.Process.HasExited) {
        Stop-Process -Id $victim.Process.Id
    }
}
if ($Scenario -eq 'LeaveRejoin') {
    Start-Sleep -Seconds ([Math]::Max(10, [Math]::Floor($DurationSeconds / 3)))
    $victim = $runs | Where-Object Name -eq "viewer-$ViewerCount" | Select-Object -First 1
    if ($null -ne $victim -and -not $victim.Process.HasExited) {
        Stop-Process -Id $victim.Process.Id
        $victim.Process.WaitForExit()
    }
    Start-Sleep -Seconds 5
    $rejoinSeconds = [Math]::Max(30, [Math]::Floor($DurationSeconds / 2))
    # A real UI restart generates a new session/peer ID while normally reusing
    # the same UDP port. Exercise that identity replacement path explicitly.
    $rejoinArguments = New-ViewerArguments $ViewerCount $rejoinSeconds '-rejoin'
    $runs.Add((Start-HarnessProcess "viewer-$ViewerCount-rejoin" $rejoinArguments))
}

Write-Manifest $runs.ToArray()
$exitCode = 0
foreach ($run in $runs) {
    if (-not $run.Process.HasExited) {
        $run.Process.WaitForExit()
    }
    $expectedScenarioStop =
        ($Scenario -eq 'Unreachable' -and $run.Name -eq "viewer-$ViewerCount") -or
        ($Scenario -eq 'LeaveRejoin' -and $run.Name -eq "viewer-$ViewerCount")
    if ($run.Process.ExitCode -ne 0 -and -not $expectedScenarioStop -and $run.Name -notlike '*rejoin*') {
        $exitCode = 1
    }
}
Write-Summary
if (-not $script:SummaryPassed) {
    $exitCode = 1
}
exit $exitCode
