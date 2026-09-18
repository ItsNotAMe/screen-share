function Assert-RoomTimingSamples($Samples) {
    if ($null -eq $Samples -or $Samples.Count -ne 20) { throw 'Expected twenty timing samples.' }
    foreach ($sample in $Samples) {
        # Windows PowerShell 5.1 uses Decimal for JSON fractional numbers;
        # PowerShell 7 uses Double. Neither strings nor booleans are evidence.
        if ($sample -isnot [double] -and $sample -isnot [decimal] -and
            $sample -isnot [int] -and $sample -isnot [long]) { throw 'Invalid timing sample.' }
        if ([double]::IsNaN($sample) -or [double]::IsInfinity($sample) -or $sample -lt 0) {
            throw 'Invalid timing sample.'
        }
    }
}

function Assert-RoomLoadNumber($Value) {
    if (($Value -isnot [double] -and $Value -isnot [decimal] -and $Value -isnot [int] -and $Value -isnot [long]) -or
        [double]::IsNaN($Value) -or [double]::IsInfinity($Value) -or $Value -lt 0) { throw 'Invalid load number.' }
}
function Assert-RoomLoadEvidence($Metrics, [string]$Role, [int]$Seconds, [string]$Decoder = 'hardware') {
    if ($Decoder -cnotin @('hardware','software') -or $Metrics.decoderMode -isnot [string] -or $Metrics.decoderMode -cne $Decoder) { throw 'Unexpected decoder selection.' }
    if ($Metrics.hardwareOnly -isnot [bool] -or $Metrics.hardwareOnly -ne ($Decoder -eq 'hardware')) { throw 'Mislabeled codec mode.' }
    foreach ($name in @('passed','runtimeReleased')) {
        if ($Metrics.$name -isnot [bool] -or -not $Metrics.$name) { throw 'Missing completed load assertion.' }
    }
    if ($Metrics.role -cne $Role -or $Metrics.width -ne 1920 -or $Metrics.height -ne 1080 -or
        $Metrics.fpsLimit -ne 60 -or $Metrics.bitrateLimitBps -ne 12000000) { throw 'Unexpected load configuration.' }
    foreach ($name in @('physicalInput','audibleOutput','externalLatencyVerified')) {
        if ($Metrics.$name -isnot [bool] -or $Metrics.$name) { throw 'Unexpected physical effects or latency claim.' }
    }
    foreach ($name in @('measuredSeconds','cpuCorePercent','frames','freshFrames','invalidFrames','freshFps')) { Assert-RoomLoadNumber $Metrics.$name }
    if ($Metrics.measuredSeconds -lt $Seconds -or $Metrics.measuredSeconds -gt $Seconds + 30 -or
        $Metrics.samples.Count -lt $Seconds) { throw 'Insufficient load duration.' }
    foreach ($name in @('privateBytes','workingSetBytes','handles')) { Assert-RoomLoadNumber $Metrics.afterStopResources.$name }
    if ($Metrics.afterStopResources.privateBytes -le 0 -or $Metrics.afterStopResources.handles -le 0) { throw 'Missing post-stop resources.' }
    $index = 0; $previousFresh = 0
    foreach ($sample in $Metrics.samples) {
        if ($sample.second -ne ++$index) { throw 'Invalid sample order.' }
        foreach ($name in @('privateBytes','workingSetBytes','handles','activePeers')) { Assert-RoomLoadNumber $sample.$name }
        if ($sample.privateBytes -le 0 -or $sample.handles -le 0) { throw 'Missing resources.' }
        if ($Role -eq 'host') {
            Assert-RoomLoadNumber $sample.hardwareFrames
            Assert-RoomLoadNumber $sample.softwareFallbacks
            if ($sample.softwareFallbacks -ne 0 -or $sample.hardwareFrames -le 0) { throw 'Hardware encoder fell back.' }
            if ($sample.activePeers -eq 1) {
                if ($sample.encoder -isnot [string] -or $sample.decoder -isnot [string] -or
                    $sample.encoder -cne 'mf-h264-hardware' -or $sample.decoder -cne "mf-h264-$Decoder" -or
                    $sample.receiverWidth -ne 1920 -or $sample.receiverHeight -ne 1080) { throw 'Missing hardware endpoint evidence.' }
            } elseif ($sample.activePeers -ne 0 -or $index -ne $Metrics.samples.Count) { throw 'Unexpected peer loss.' }
        } else {
            if ($sample.activePeers -ne 1) { throw 'Viewer lost its host.' }
            foreach ($name in @('frames','freshFrames','invalidFrames')) { Assert-RoomLoadNumber $sample.video.$name }
            if ($sample.video.freshFrames -le $previousFresh -or $sample.video.freshFrames -gt $sample.video.frames -or
                $sample.video.invalidFrames -ne 0) { throw 'Viewer stalled or delivered invalid images.' }
            $previousFresh = $sample.video.freshFrames
        }
    }
    if ($Role -eq 'host') {
        foreach ($name in @('hardwareEncoderObserved','decoderObserved')) {
            if ($Metrics.$name -isnot [bool] -or -not $Metrics.$name) { throw 'Missing hardware use.' }
        }
        foreach ($name in @('hardwareDecoderObserved','hardwareOnly')) {
            if ($Metrics.$name -isnot [bool] -or $Metrics.$name -ne ($Decoder -eq 'hardware')) { throw 'Mislabeled decoder evidence.' }
        }
    } elseif ($Role -eq 'viewer') {
        if ($Metrics.freshFrames -gt $Metrics.frames -or $Metrics.freshFps -lt 45 -or $Metrics.invalidFrames -ne 0 -or
            [Math]::Abs($Metrics.freshFps - $Metrics.freshFrames / $Metrics.measuredSeconds) -gt 0.01) { throw 'Insufficient valid fresh 1080p video.' }
    } else { throw 'Invalid load role.' }
}
