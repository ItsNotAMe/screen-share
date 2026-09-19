# Local signing helpers. Never send a passphrase through arguments or logs.
function Invoke-OpenSslWithSecret {
    param([string]$OpenSsl, [string[]]$Arguments, [Security.SecureString]$Secret)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $OpenSsl
    $quoted = foreach ($argument in $Arguments) {
        if ($argument.Contains('"')) { throw 'Invalid quote in OpenSSL argument.' }
        '"' + $argument + '"'
    }
    $start.Arguments = $quoted -join ' '
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $pointer = [IntPtr]::Zero
    try {
        $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Secret)
        $process.StandardInput.WriteLine([Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer))
        $process.StandardInput.Close()
        $null = $process.StandardOutput.ReadToEnd()
        $null = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw 'OpenSSL signing-key operation failed. Check the key and stored passphrase.' }
    } finally {
        if ($pointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
        $process.Dispose()
    }
}

function Assert-PinnedUpdatePublicKey {
    param([string]$PublicKeyPath)
    $source = Get-Content (Join-Path $PSScriptRoot '../frontend/ui/UpdateManager.cpp') -Raw
    $block = [regex]::Match($source, 'kUpdatePublicKeyXy\s*=\s*\{([^}]+)\}', 'Singleline').Groups[1].Value
    [byte[]]$expected = @([regex]::Matches($block, '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) })
    [byte[]]$der = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $PublicKeyPath).Path)
    if ($expected.Length -ne 64 -or $der.Length -ne 91 -or
        [Convert]::ToBase64String($der[27..90]) -ne [Convert]::ToBase64String($expected)) {
        throw 'The release public key does not match the key trusted by ScreenShare. Do not replace the existing signing identity.'
    }
}
