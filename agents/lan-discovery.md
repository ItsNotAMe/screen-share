# LAN Discovery Notes

## Current Shape

- `src/transport/LanDiscovery.*` implements a small IPv4 UDP query/response helper.
- Default discovery port: `47995`.
- Receiver side opt-in: `--watch PORT --lan-advertise` or `--udp-recv PORT --lan-advertise`.
- Sender side scan: `--lan-discover --lan-discover-seconds S`.
- UI Watch enables `--lan-advertise` by default. UI Share uses `--lan-discover` and parses the first `share_target=HOST:PORT` line to fill address/port.
- PR #59 is being simplified to access-code-only discovery polish. No separate invite/pair code should remain.

## Protocol

- Discovery sends a short query to `255.255.255.255:47995`, `127.0.0.1:47995`, and directed broadcast addresses from active IPv4 adapters such as `192.168.1.255`.
- Tailscale and similar mesh VPNs are not expected to appear here because they route unicast traffic but generally do not forward LAN broadcast discovery. The UI has a separate optional Tailscale peer picker using `tailscale status --json`; keep that separate from this UDP broadcast protocol.
- Advertisers reply directly to the sender with:
  - advertised watch/share port,
  - display name,
  - session ID,
  - session fingerprint,
  - access-code fingerprint when the receiver expects encrypted traffic.
- `--lan-discover` reports `security=encrypted` or `security=plaintext`.
- For encrypted receivers, the generated command includes `--access-code CODE` as a placeholder and never broadcasts the raw code.
- UI Find on LAN can compare the typed access code against the advertised fingerprint and warn when it does not match.
- For plaintext receivers, the generated command includes `--allow-plaintext`.
- This is only local-network convenience. It is not authentication, a raw access-code exchange, or Internet/NAT traversal.

## Next Discovery/Security Work

- After this branch, the next discovery/security step is Internet/NAT traversal design. Keep the local access-code flow as the base assumption.

## Testing

```powershell
$receiver = Start-Process -FilePath '.\build\debug\ScreenShare.exe' -ArgumentList @('--udp-recv','50130','--seconds','5','--lan-advertise','--session','lan-test','--lan-name','TestReceiver','--access-code','LANSEC-TEST') -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500
.\build\debug\ScreenShare.exe --lan-discover --lan-discover-seconds 2
Wait-Process -Id $receiver.Id -Timeout 10
```
