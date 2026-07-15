param(
    [string]$OutputDirectory = (Join-Path ([Environment]::GetFolderPath("UserProfile")) ".screenshare-release"),
    [string]$OpenSslPath = "openssl"
)

$ErrorActionPreference = "Stop"

$openssl = Get-Command $OpenSslPath -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$privateKeyPath = Join-Path $OutputDirectory "screenshare-update.key"
$publicKeyPath = Join-Path $OutputDirectory "screenshare-update-public.der"
if (Test-Path -LiteralPath $privateKeyPath) {
    throw "Refusing to replace existing signing key: $privateKeyPath"
}

Write-Host "Generating an encrypted ECDSA P-256 update-signing key."
Write-Host "Choose a strong passphrase and store it in your password manager."
& $openssl.Source genpkey `
    -algorithm EC `
    -pkeyopt ec_paramgen_curve:P-256 `
    -aes-256-cbc `
    -out $privateKeyPath
if ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to generate the private key."
}

& $openssl.Source pkey `
    -in $privateKeyPath `
    -pubout `
    -outform DER `
    -out $publicKeyPath
if ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to export the public key."
}

$publicDer = [IO.File]::ReadAllBytes($publicKeyPath)
if ($publicDer.Length -lt 65 -or $publicDer[$publicDer.Length - 65] -ne 0x04) {
    throw "OpenSSL produced an unexpected P-256 public-key encoding."
}

$publicKeyXy = $publicDer[($publicDer.Length - 64)..($publicDer.Length - 1)]
$initializer = ($publicKeyXy | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "

Write-Host "Private key: $privateKeyPath"
Write-Host "Public key:  $publicKeyPath"
Write-Host "Back up the encrypted private key securely; never commit or share it."
Write-Host "Pinned public-key initializer (safe to commit):"
Write-Output $initializer
