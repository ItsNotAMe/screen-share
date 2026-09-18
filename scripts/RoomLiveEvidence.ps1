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
