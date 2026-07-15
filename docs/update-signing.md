# Signing auto-update manifests

The updater refuses to install anything unless the update manifest carries a
valid signature from a pinned release key. This stops a compromised release
host/CDN (or a MITM that somehow bypasses TLS) from pushing a malicious build:
the SHA-256 in the manifest is only trusted because the signature binds it to a
key an attacker does not hold.

The algorithm is **ECDSA P-256 over SHA-256** (Windows CNG native; Ed25519 is
not available in CNG without bundling a third-party implementation). The key
property — a signed manifest verified against a public key pinned in the binary
before install — is identical.

**Until a key is pinned, auto-update is disabled (fail closed).** The pinned key
lives in `kUpdatePublicKeyXy` in `src/ui/UpdateManager.cpp` (all zero by
default).

## 1. Generate the signing keypair (once)

Run the helper from PowerShell. OpenSSL prompts for a passphrase and writes the
encrypted private key outside the repository by default:

```powershell
.\scripts\new-update-signing-key.ps1
```

Back up `%USERPROFILE%\.screenshare-release\screenshare-update.key` securely and
store its passphrase in a password manager. Never commit or share the private
key. The script prints the raw 64-byte public-key initializer to pin in
`kUpdatePublicKeyXy`.

Equivalent manual OpenSSL commands:

```sh
# Encrypted private key — keep OFFLINE and SECRET. Never commit it.
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
  -aes-256-cbc -out screenshare-update.key

# Raw 64-byte public key (X||Y) as a C initializer to paste into
# kUpdatePublicKeyXy in src/ui/UpdateManager.cpp:
openssl ec -in screenshare-update.key -pubout -outform DER 2>/dev/null \
  | tail -c 64 | xxd -i
```

Paste the emitted bytes into `kUpdatePublicKeyXy` and rebuild.

## 2. Sign each release manifest

The signed message is exactly these three fields joined by newlines (must match
`assets.portableZip.url` / `.sha256` and the top-level `version` in the
manifest):

```
<version>\n<packageUrl>\n<sha256>
```

After generating the unsigned intermediate manifest, use the helper to build
the exact message, prompt for the encrypted key's passphrase, convert OpenSSL's
DER signature to raw `R||S`, and add the base64 signature to the manifest:

```powershell
.\scripts\sign-update-manifest.ps1 `
  -ManifestPath .\build\release\screenshare-update.json `
  -PrivateKeyPath "$env:USERPROFILE\.screenshare-release\screenshare-update.key" `
  -PublicKeyPath "$env:USERPROFILE\.screenshare-release\screenshare-update-public.der"
```

The public-key argument makes the helper verify the signature before updating
the manifest. The lower-level manual process is retained below for recovery or
use on a separate signing machine.

```sh
# message.txt = the three fields, newline-separated, no trailing newline
printf '%s\n%s\n%s' "$VERSION" "$PACKAGE_URL" "$SHA256" > message.txt

# Sign, then convert the DER signature to the raw 64-byte R||S form and base64:
openssl dgst -sha256 -sign screenshare-update.key -out sig.der message.txt
python - <<'PY'
import base64
sig=open('sig.der','rb').read()
i=2+((sig[1]&0x7f) if sig[1]&0x80 else 0)
rlen=sig[i+1]; r=sig[i+2:i+2+rlen]; j=i+2+rlen; slen=sig[j+1]; s=sig[j+2:j+2+slen]
raw=r.lstrip(b'\x00').rjust(32,b'\x00')+s.lstrip(b'\x00').rjust(32,b'\x00')
print(base64.b64encode(raw).decode())
PY
```

Put the printed base64 string in the manifest as a top-level `"signature"`
field:

```json
{
  "version": "1.4.2",
  "assets": { "portableZip": { "url": "https://…/pkg.zip", "sha256": "…", "size": 12345 } },
  "signature": "<base64 R||S>"
}
```

The `signature` field itself is not part of the signed message, so adding it to
the manifest does not invalidate the signature.
