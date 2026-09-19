[CmdletBinding()]
param(
    [string]$SigningDirectory = "$env:USERPROFILE/.screenshare-release",
    [string]$OpenSslPath = 'openssl'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/UpdateSigning.ps1"
$privateKey = Join-Path $SigningDirectory 'screenshare-update.key'
$publicKey = Join-Path $SigningDirectory 'screenshare-update-public.der'
Assert-PinnedUpdatePublicKey $publicKey
if (-not (Test-Path -LiteralPath $privateKey)) { throw 'Restore the existing encrypted release key first.' }
$secret = Read-Host 'Enter the existing ScreenShare signing-key passphrase (stored for this Windows account only)' -AsSecureString
if ($secret.Length -eq 0) { throw 'Passphrase cannot be empty.' }
$temporaryPublic = Join-Path $env:TEMP ('screenshare-public-' + [Guid]::NewGuid().ToString('N') + '.der')
try {
    Invoke-OpenSslWithSecret -OpenSsl (Get-Command $OpenSslPath -ErrorAction Stop).Source `
        -Arguments @('pkey', '-in', $privateKey, '-passin', 'stdin', '-pubout', '-outform', 'DER', '-out', $temporaryPublic) -Secret $secret
    Assert-PinnedUpdatePublicKey $temporaryPublic
    $destination = Join-Path $SigningDirectory 'update-passphrase.dpapi'
    $secret | ConvertFrom-SecureString | Set-Content -LiteralPath $destination -Encoding ASCII -NoNewline
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl = [Security.AccessControl.FileSecurity]::new()
    $acl.SetOwner($identity)
    $acl.SetAccessRuleProtection($true, $false)
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($identity, 'FullControl', 'Allow'))
    Set-Acl -LiteralPath $destination -AclObject $acl
    Write-Host 'Signing is configured. Future release builds can sign automatically under this Windows account.'
} finally {
    $secret.Dispose()
    Remove-Item -LiteralPath $temporaryPublic -Force -ErrorAction SilentlyContinue
}
