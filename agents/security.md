# Security Notes

## Local Access Code

- PR #55 merged this first gate.
- CLI/UI option: `--access-code CODE` on both Share and Watch sides.
- The code is validated locally, converted to a non-cryptographic 64-bit fingerprint, and never printed raw.
- Video, audio, and receiver feedback packets carry the fingerprint so mismatched sessions can be rejected.
- Saved reports and the Qt command preview redact the raw access code.
- Receiver telemetry should report `access_rejected_datagrams` so bad-code or wrong-sender cases are visible.
- Sender telemetry should report `udp_feedback_access_rejected` if feedback comes back with the wrong access fingerprint.

## Encrypted Local Sessions

- PR #56 merged this layer.
- The access code now also derives a 256-bit UDP encryption key with PBKDF2-HMAC-SHA256 and Windows CNG.
- Video payloads, audio payloads, and receiver feedback payloads are encrypted with AES-GCM.
- Packet routing and diagnostics metadata stays plaintext: sizes, timestamps, fragment IDs, access-code fingerprint, flags, nonce, and tag.
- Because the access-code fingerprint is visible metadata, short codes are still guessable; recommend long/random codes for untrusted LANs.
- Receiver rejects plaintext packets when an access-code encryption key is configured and reports `crypto_rejected_datagrams`.
- Sender rejects plaintext or undecryptable feedback when an access-code encryption key is configured and reports `udp_feedback_crypto_rejected`.
- Saved reports include `UDP encryption: yes/no` and must never include the raw access code.

## Security UX

- PR #57 merged this layer.
- CLI adds `--generate-access-code` for CNG-generated 20-character base32-ish codes grouped with dashes.
- CLI adds `--allow-plaintext` to explicitly acknowledge unencrypted UDP mode and suppress the plaintext warning.
- Qt UI Worker rooms let the engine receive a random room access key from signaling and use it as `--access-code` automatically. Users do not type an access code for normal rooms.
- Qt UI manual/Nearby/fallback flows still use the visible access-code field or explicit plaintext checkbox.
- Qt UI can generate and copy a manual access code; command preview continues to redact the raw code.
- Qt UI should surface bad access-code paths without exposing secrets: invite decrypt failures, fingerprint mismatches, and rejected-packet counters clear/focus the access-code field and warn once per run.
- LAN discovery should stay access-code-only for now. It can advertise and compare fingerprints, but it must not broadcast raw access codes.

## Room-Key Direction

- Worker rooms hide "access code" from normal users.
- Rooms are encrypted by default even when the user does not type a password.
- The signaling Durable Object generates a random hidden UDP access key for no-password public rooms. This encrypts UDP media without a user-visible access code, but anyone allowed to join the public room can receive the key.
- A visible room password is optional. When present, the Worker verifies it over HTTPS using a stored salted verifier before returning the room access key.
- The native app still mixes the room password locally into the hidden room access key before deriving UDP crypto keys. The Worker must not store plaintext room passwords.

## LAN Invite Metadata

- PR #58 merged this layer.
- LAN discovery now advertises whether the receiver expects encrypted or plaintext UDP traffic.
- Encrypted discovery responses include the access-code fingerprint, not the raw access code.
- Share-side discovery output uses `--access-code CODE` as a placeholder for encrypted receivers and `--allow-plaintext` for plaintext receivers.
- UI Find on LAN fills the session ID, toggles plaintext acknowledgement for plaintext receivers, warns when the discovered receiver requires an access code, and compares a typed access code against the advertised fingerprint.

## NAT Invite Metadata

- NAT invite/probe diagnostics use the same access-code model as local UDP sessions.
- `--make-invite` requires either `--access-code CODE` or `--allow-plaintext`.
- The invite prints only metadata: public/local endpoints, session ID/fingerprint, security mode, access-code fingerprint, and expiry. It must never print the raw access code.
- `--nat-probe` rejects encrypted peer invites unless the typed access code matches the peer access-code fingerprint.
- Current NAT probe packets are diagnostics, not encrypted media. They carry session/access fingerprints as routing/setup metadata only.
- Share `--local-invite` also validates the local invite access-code fingerprint before binding the sender socket to the invite's local port.
