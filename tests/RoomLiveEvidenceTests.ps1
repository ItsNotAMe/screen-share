$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/RoomLiveEvidence.ps1')
$samples = (('[' + ((1..20 | ForEach-Object { '42.125' }) -join ',') + ']') | ConvertFrom-Json)
Assert-RoomTimingSamples $samples
Assert-RoomTimingSamples @((1..20 | ForEach-Object { [double]42.125 }))
Assert-RoomTimingSamples @((1..20 | ForEach-Object { [decimal]42.125 }))
$rejected = 0
foreach ($bad in @('42.125', $true, $null, -1, [double]::NaN, [double]::PositiveInfinity)) {
    $values = @($samples); $values[0] = $bad
    try { Assert-RoomTimingSamples $values } catch { ++$rejected }
}
foreach ($badCount in @(0, 19, 21)) {
    $values = @(); for ($i = 0; $i -lt $badCount; ++$i) { $values += 1 }
    try { Assert-RoomTimingSamples $values } catch { ++$rejected }
}
if ($rejected -ne 9) { throw "Timing validator accepted malformed evidence ($rejected/9 rejected)." }
Write-Output 'Timing evidence: JSON/Decimal/Double accepted; nine malformed cases rejected.'

function LoadFixture([string]$role) {
    $metrics = [ordered]@{passed=$true; runtimeReleased=$true; sessionHealthy=$true; role=$role; width=1920; height=1080; fpsLimit=60; bitrateLimitBps=12000000
        physicalInput=$false; audibleOutput=$false; externalLatencyVerified=$false; measuredSeconds=10.0; cpuCorePercent=30.0
        frames=500; freshFrames=500; freshFps=50.0; invalidFrames=0; hardwareEncoderObserved=$true; hardwareDecoderObserved=$true; hardwareOnly=$true
        decoderMode='hardware'; decoderObserved=$true; consumerMode='pixels'
        afterStopResources=@{privateBytes=100000;workingSetBytes=100000;handles=100}
        samples=@(1..10 | ForEach-Object { @{second=$_;privateBytes=100000;workingSetBytes=100000;handles=100;activePeers=1
            hardwareFrames=100+$_*50;softwareFallbacks=0;encoder='mf-h264-hardware';decoder='mf-h264-hardware';receiverWidth=1920;receiverHeight=1080
            video=@{frames=$_*50;freshFrames=$_*50;invalidFrames=0}} })}
    return ($metrics | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}
Assert-RoomLoadEvidence (LoadFixture 'host') 'host' 10
Assert-RoomLoadEvidence (LoadFixture 'viewer') 'viewer' 10
$badCases = @(
    {param($m) $m.passed='true'},
    {param($m) $m.runtimeReleased=$false},
    {param($m) $m.externalLatencyVerified=$true},
    {param($m) $m.physicalInput=$true},
    {param($m) $m.width=1280},
    {param($m) $m.measuredSeconds=9},
    {param($m) $m.cpuCorePercent=[double]::NaN},
    {param($m) $m.samples[2].handles=$null},
    {param($m) $m.samples[2].second=1},
    {param($m) $m.samples[2].softwareFallbacks=1},
    {param($m) $m.samples[2].decoder='mf-h264-software'},
    {param($m) $m.samples[2].activePeers=0},
    {param($m) $m.hardwareOnly=$false},
    {param($m) $m.afterStopResources.privateBytes=0}
)
$loadRejected=0
foreach ($change in $badCases) {
    $m=LoadFixture 'host'; & $change $m
    try { Assert-RoomLoadEvidence $m 'host' 10 } catch { ++$loadRejected }
}
foreach ($change in @(
    {param($m) $m.freshFps=44},
    {param($m) $m.freshFrames=600},
    {param($m) $m.invalidFrames=1},
    {param($m) $m.samples[2].video.freshFrames=100}
)) {
    $m=LoadFixture 'viewer'; & $change $m
    try { Assert-RoomLoadEvidence $m 'viewer' 10 } catch { ++$loadRejected }
}
if ($loadRejected -ne 18) { throw "Load validator accepted malformed evidence ($loadRejected/18 rejected)." }
Write-Output 'Load evidence: host/viewer accepted; eighteen malformed, fallback and stalled cases rejected.'
$software = LoadFixture 'host'; $software.decoderMode='software'; $software.hardwareDecoderObserved=$false; $software.hardwareOnly=$false
foreach ($sample in $software.samples) { $sample.decoder='mf-h264-software' }
Assert-RoomLoadEvidence $software 'host' 10 'software'
$modeRejected=0
try { Assert-RoomLoadEvidence $software 'host' 10 } catch { ++$modeRejected }
$software.samples[3].decoder='mf-h264-hardware'
try { Assert-RoomLoadEvidence $software 'host' 10 'software' } catch { ++$modeRejected }
$software.samples[3].decoder='mf-h264-software'; $software.decoderObserved=$false
try { Assert-RoomLoadEvidence $software 'host' 10 'software' } catch { ++$modeRejected }
$software.decoderObserved=$true; $software.decoderMode=$true
try { Assert-RoomLoadEvidence $software 'host' 10 'software' } catch { ++$modeRejected }
$software.decoderMode='software'; $software.samples[3].decoder=$true
try { Assert-RoomLoadEvidence $software 'host' 10 'software' } catch { ++$modeRejected }
if ($modeRejected -ne 5) { throw 'Decoder mode evidence accepted mislabeled or missing observations.' }
Write-Output 'Explicit software decode accepted; five mislabeled/missing decoder cases rejected.'

function PresentationFixture {
    $m = LoadFixture 'viewer'; $m.consumerMode = 'presentation'
    $m | Add-Member presentationRecovery ([pscustomobject]@{passed=$true; resizeSmall=$true; resizeLarge=$true; minimizedBounded=$true; restored=$true})
    $m | Add-Member postRecoveryVideo ([pscustomobject]@{frames=550; freshFrames=550; invalidFrames=0})
    $m | Add-Member presentation ([pscustomobject]@{presented=800; measuredPresented=500; presentedFps=50.0; errors=0; maximumFrameLatency=1; hardwareAccelerated=$true})
    foreach ($sample in $m.samples) {
        $sample | Add-Member presentation ([pscustomobject]@{presented=300+$sample.second*50; errors=0; maximumFrameLatency=1; pending=1; maxWaitUs=2000; hardwareAccelerated=$true})
    }
    return $m
}
Assert-RoomLoadEvidence (PresentationFixture) 'viewer' 10 'hardware' 'presentation'
$presentationRejected = 0
foreach ($change in @(
    {param($m) $m.consumerMode='pixels'},
    {param($m) $m.consumerMode=$true},
    {param($m) $m.presentation=$null},
    {param($m) $m.presentation.presentedFps=44},
    {param($m) $m.presentation.measuredPresented=900},
    {param($m) $m.presentation.maximumFrameLatency=2},
    {param($m) $m.presentation.errors=1},
    {param($m) $m.sessionHealthy=$false},
    {param($m) $m.presentationRecovery=$null},
    {param($m) $m.presentationRecovery.restored='true'},
    {param($m) $m.presentationRecovery.minimizedBounded=$false},
    {param($m) $m.postRecoveryVideo.invalidFrames=1},
    {param($m) $m.postRecoveryVideo.freshFrames=500},
    {param($m) $m.presentation.hardwareAccelerated='true'},
    {param($m) $m.samples[3].presentation.hardwareAccelerated=$false},
    {param($m) $m.samples[3].presentation.presented=1},
    {param($m) $m.samples[3].presentation.pending=2},
    {param($m) $m.samples[3].presentation.maxWaitUs='2000'},
    {param($m) $m.samples[3].presentation=$null}
)) {
    $m = PresentationFixture; & $change $m
    try { Assert-RoomLoadEvidence $m 'viewer' 10 'hardware' 'presentation' } catch { ++$presentationRejected }
}
if ($presentationRejected -ne 19) { throw 'Presentation evidence accepted a missing, stalled or mislabeled result.' }
Write-Output 'Presentation accepted; nineteen missing, stalled or mislabeled cases rejected.'
