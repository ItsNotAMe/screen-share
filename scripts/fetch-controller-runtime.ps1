[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\.cache\controller')
)

$ErrorActionPreference = 'Stop'
$driverUrl = 'https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe'
$driverName = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
$expectedSha256 = '89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A'

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
$driverPath = Join-Path $resolvedOutput $driverName
$downloadPath = "$driverPath.download"

if (Test-Path -LiteralPath $driverPath) {
    $existingHash = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash
    if ($existingHash -eq $expectedSha256) {
        Write-Host "Pinned controller runtime is already present: $driverPath"
        Write-Output $driverPath
        exit 0
    }
    Remove-Item -LiteralPath $driverPath -Force
}

try {
    Invoke-WebRequest -Uri $driverUrl -OutFile $downloadPath
    $actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedSha256) {
        throw "Controller runtime SHA-256 mismatch. Expected $expectedSha256, got $actualHash."
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $downloadPath
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid) {
        throw "Controller runtime signature is not valid: $($signature.StatusMessage)"
    }
    if ($signature.SignerCertificate.Subject -notlike '*Nefarius Software Solutions*') {
        throw "Controller runtime signer was unexpected: $($signature.SignerCertificate.Subject)"
    }

    Move-Item -LiteralPath $downloadPath -Destination $driverPath -Force
}
finally {
    if (Test-Path -LiteralPath $downloadPath) {
        Remove-Item -LiteralPath $downloadPath -Force
    }
}

Write-Host "Verified controller runtime: $driverPath"
Write-Output $driverPath
