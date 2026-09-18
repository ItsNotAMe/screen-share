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
