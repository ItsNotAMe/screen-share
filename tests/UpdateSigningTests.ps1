$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/UpdateSigning.ps1"
$directory = Join-Path ([IO.Path]::GetTempPath()) ('screenshare-signing-test-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($directory) | Out-Null
$secret = ConvertTo-SecureString 'synthetic-test-passphrase' -AsPlainText -Force
try {
    $openssl = (Get-Command openssl -ErrorAction Stop).Source
    $private = Join-Path $directory 'test.key'
    $public = Join-Path $directory 'public.der'
    Invoke-OpenSslWithSecret $openssl @('genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-aes-256-cbc','-pass','stdin','-out',$private) $secret
    Invoke-OpenSslWithSecret $openssl @('pkey','-in',$private,'-passin','stdin','-pubout','-outform','DER','-out',$public) $secret
    $stored = Join-Path $directory 'passphrase.dpapi'
    # Retain a newline to cover Windows PowerShell's default Set-Content output.
    $secret | ConvertFrom-SecureString | Set-Content $stored -Encoding ASCII
    [IO.File]::WriteAllBytes((Join-Path $directory 'package.zip'),[byte[]](1,2,3))
    [IO.File]::WriteAllBytes((Join-Path $directory 'setup.exe'),[byte[]](4,5,6))
    $manifest = Join-Path $directory 'manifest.json'
    & "$PSScriptRoot/../scripts/create-update-manifest.ps1" -Version 0.0.0 -ZipPath "$directory/package.zip" -InstallerPath "$directory/setup.exe" -OutputPath $manifest | Out-Null
    & "$PSScriptRoot/../scripts/sign-update-manifest.ps1" -ManifestPath $manifest -PrivateKeyPath $private -PublicKeyPath $public -PassphraseFile $stored | Out-Null
    $data = Get-Content $manifest -Raw | ConvertFrom-Json
    [byte[]]$der = [IO.File]::ReadAllBytes($public)
    [byte[]]$blob = @(0x45,0x43,0x53,0x31,32,0,0,0) + $der[27..90]
    $key = [Security.Cryptography.CngKey]::Import($blob,[Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    $ecdsa = [Security.Cryptography.ECDsaCng]::new($key)
    try {
        foreach ($kind in @('portableZip','windowsInstaller')) {
            $asset = $data.assets.$kind
            [byte[]]$message = [Text.Encoding]::UTF8.GetBytes("0.0.0`n$($asset.url)`n$($asset.sha256)")
            [byte[]]$signature = [Convert]::FromBase64String($asset.signature)
            if (-not $ecdsa.VerifyData($message,$signature,[Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "Invalid $kind signature" }
            $message[0] = 49
            if ($ecdsa.VerifyData($message,$signature,[Security.Cryptography.HashAlgorithmName]::SHA256)) { throw 'Tampered message accepted' }
        }
    } finally { $ecdsa.Dispose(); $key.Dispose() }
    $rejected = $false
    try { Assert-PinnedUpdatePublicKey $public } catch { $rejected = $true }
    if (-not $rejected) { throw 'Untrusted signing identity accepted' }
    Write-Host 'Automatic signing, both asset signatures, tamper rejection and key pinning passed.'
} finally {
    $secret.Dispose()
    $resolved = [IO.Path]::GetFullPath($directory)
    $expected = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\screenshare-signing-test-'
    if (-not $resolved.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
