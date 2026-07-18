[CmdletBinding(DefaultParameterSetName = "PrivateKey")]
param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,

    [Parameter(Mandatory = $true, ParameterSetName = "PrivateKey")]
    [string]$PrivateKeyPath,

    [Parameter(Mandatory = $true, ParameterSetName = "ExistingSignature")]
    [string]$SignatureDerPath,

    [Parameter(ParameterSetName = "ExistingSignature")]
    [string]$InstallerSignatureDerPath,

    [string]$PublicKeyPath,
    [string]$OpenSslPath = "openssl"
)

$ErrorActionPreference = "Stop"

function Read-DerLength {
    param(
        [byte[]]$Data,
        [ref]$Offset
    )

    if ($Offset.Value -ge $Data.Length) {
        throw "Truncated DER length."
    }
    $first = $Data[$Offset.Value]
    $Offset.Value++
    if (($first -band 0x80) -eq 0) {
        return [int]$first
    }

    $count = $first -band 0x7f
    if ($count -eq 0 -or $count -gt 4 -or $Offset.Value + $count -gt $Data.Length) {
        throw "Invalid DER length."
    }

    $length = 0
    for ($i = 0; $i -lt $count; $i++) {
        $length = ($length -shl 8) -bor $Data[$Offset.Value]
        $Offset.Value++
    }
    return $length
}

function Read-DerInteger32 {
    param(
        [byte[]]$Data,
        [ref]$Offset
    )

    if ($Offset.Value -ge $Data.Length -or $Data[$Offset.Value] -ne 0x02) {
        throw "Expected a DER INTEGER."
    }
    $Offset.Value++
    $length = Read-DerLength -Data $Data -Offset $Offset
    if ($length -lt 1 -or $Offset.Value + $length -gt $Data.Length) {
        throw "Invalid DER INTEGER length."
    }

    $start = $Offset.Value
    $Offset.Value += $length
    $hadSignPadding = $false
    if ($length -eq 33) {
        if ($Data[$start] -ne 0) {
            throw "Oversized DER INTEGER."
        }
        $hadSignPadding = $true
        $start++
        $length--
    }
    if ($length -gt 32 -or ((-not $hadSignPadding) -and ($Data[$start] -band 0x80) -ne 0)) {
        throw "Invalid ECDSA integer encoding."
    }

    $result = [byte[]]::new(32)
    [Array]::Copy($Data, $start, $result, 32 - $length, $length)
    return $result
}

function Convert-DerEcdsaSignatureToRaw {
    param([byte[]]$Data)

    $offset = 0
    if ($Data.Length -lt 2 -or $Data[$offset] -ne 0x30) {
        throw "Expected a DER SEQUENCE."
    }
    $offset++
    $sequenceLength = Read-DerLength -Data $Data -Offset ([ref]$offset)
    if ($offset + $sequenceLength -ne $Data.Length) {
        throw "Invalid DER ECDSA signature length."
    }

    [byte[]]$r = Read-DerInteger32 -Data $Data -Offset ([ref]$offset)
    [byte[]]$s = Read-DerInteger32 -Data $Data -Offset ([ref]$offset)
    if ($offset -ne $Data.Length) {
        throw "Unexpected trailing data in DER ECDSA signature."
    }

    $raw = [byte[]]::new(64)
    [Array]::Copy($r, 0, $raw, 0, 32)
    [Array]::Copy($s, 0, $raw, 32, 32)
    return $raw
}

$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -Raw -LiteralPath $resolvedManifest | ConvertFrom-Json
$version = [string]$manifest.version
if (-not $version) {
    throw "Manifest is missing a version."
}

$assetKeys = @("portableZip", "windowsInstaller")
$signedAssetCount = 0
$portableSignatureBase64 = $null
$openssl = Get-Command $OpenSslPath -ErrorAction Stop
$resolvedPrivateKey = $null
$resolvedPublicKey = $null
if ($PSCmdlet.ParameterSetName -eq "PrivateKey") {
    $resolvedPrivateKey = (Resolve-Path -LiteralPath $PrivateKeyPath).Path
}
if ($PublicKeyPath) {
    $resolvedPublicKey = (Resolve-Path -LiteralPath $PublicKeyPath).Path
}

foreach ($assetKey in $assetKeys) {
    $asset = $manifest.assets.$assetKey
    if (-not $asset) {
        continue
    }

    $packageUrl = [string]$asset.url
    $sha256 = [string]$asset.sha256
    if (-not $packageUrl -or $sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Manifest asset '$assetKey' is missing a valid URL or SHA-256."
    }

    $message = "$version`n$packageUrl`n$($sha256.ToLowerInvariant())"
    $messagePath = Join-Path ([IO.Path]::GetTempPath()) ("screenshare-update-message-" + [Guid]::NewGuid().ToString("N") + ".txt")
    $temporarySignature = $null
    try {
        [IO.File]::WriteAllText($messagePath, $message, [Text.UTF8Encoding]::new($false))

        if ($PSCmdlet.ParameterSetName -eq "PrivateKey") {
            $temporarySignature = Join-Path ([IO.Path]::GetTempPath()) ("screenshare-update-signature-" + [Guid]::NewGuid().ToString("N") + ".der")
            Write-Host "Signing $assetKey for version $version. Enter the private-key passphrase when prompted."
            & $openssl.Source dgst -sha256 -sign $resolvedPrivateKey -out $temporarySignature $messagePath
            if ($LASTEXITCODE -ne 0) {
                throw "OpenSSL failed to sign manifest asset '$assetKey'."
            }
            $resolvedSignature = $temporarySignature
        } else {
            $providedSignature = if ($assetKey -eq "portableZip") { $SignatureDerPath } else { $InstallerSignatureDerPath }
            if (-not $providedSignature) {
                throw "InstallerSignatureDerPath is required when windowsInstaller is present."
            }
            $resolvedSignature = (Resolve-Path -LiteralPath $providedSignature).Path
        }

        if ($resolvedPublicKey) {
            & $openssl.Source dgst -sha256 -verify $resolvedPublicKey -keyform DER -signature $resolvedSignature $messagePath
            if ($LASTEXITCODE -ne 0) {
                throw "The signature for '$assetKey' does not match the manifest and public key."
            }
        }

        $signatureDer = [IO.File]::ReadAllBytes($resolvedSignature)
        [byte[]]$signatureRaw = Convert-DerEcdsaSignatureToRaw -Data $signatureDer
        $signatureBase64 = [Convert]::ToBase64String($signatureRaw)
        $asset | Add-Member -NotePropertyName signature -NotePropertyValue $signatureBase64 -Force
        if ($assetKey -eq "portableZip") {
            $portableSignatureBase64 = $signatureBase64
        }
        $signedAssetCount++

        Write-Host "Signed asset: $assetKey"
        Write-Host "  Package URL: $packageUrl"
        Write-Host "  SHA256: $($sha256.ToLowerInvariant())"
    } finally {
        Remove-Item -LiteralPath $messagePath -Force -ErrorAction SilentlyContinue
        if ($temporarySignature) {
            Remove-Item -LiteralPath $temporarySignature -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($signedAssetCount -eq 0) {
    throw "Manifest does not contain a portableZip or windowsInstaller asset."
}
if ($portableSignatureBase64) {
    # Preserve the root signature consumed by ScreenShare 0.2.3 and earlier.
    # New clients treat it only as a legacy fallback for portableZip.
    $manifest | Add-Member -NotePropertyName signature -NotePropertyValue $portableSignatureBase64 -Force
}
$json = $manifest | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($resolvedManifest, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Host "Signed and updated $resolvedManifest"
