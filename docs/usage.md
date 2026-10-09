# ScreenShare usage

The modular room backend is the default. Open `ScreenShareUi.exe` to create or
join a room. No backend switch is needed. The default service is the isolated v2
Worker with a ten-room cap; `--signal-server HTTPS_ORIGIN` selects another
compatible deployment.

## Command line

```powershell
ScreenShare --create-room --name "My room" --nickname Host --display 0
ScreenShare --join-room ROOM_ID --nickname Viewer
ScreenShare --join-room screenshare://room/v2/ROOM_ID --decoder software
ScreenShare --create-room --private --password-file room-password.txt
ScreenShare --help
```

Create/join are mutually exclusive. Hosts choose display or window capture,
audio source, preset, resolution, FPS and bitrate. Viewers choose decoder,
playback device, mute/volume and preview. `--seconds N` bounds a CLI session;
Ctrl+C requests orderly shutdown. Configuration-file automation remains available
as `ScreenShare --room-v2 CONFIG.json`.

See [CLI configuration and options](cli.md) for complete schemas and ranges.
Shared source audio is the default: window sharing captures the selected app's
audio and its child processes; display sharing captures system audio. It follows
source changes and mute/unmute. Windows isolates app processes rather than
individual windows, so other windows or tabs in the same app may be audible.
App audio needs Windows build 20348 or later (including Windows 11). If capture
fails, video continues with silence. Choose System audio to share all apps instead.
Passwords use a UTF-8 file rather than command-line text. Links carry a room ID,
not credentials or an arbitrary server address.

## Controls and diagnostics

The host can grant mouse, keyboard and controller permissions independently,
with or without a viewer request. Room admission does not grant input. The host
can revoke each permission; the panic shortcut is Ctrl+Alt+Shift+F12. In the UI,
focus loss releases held input and pauses forwarding without cancelling grants.
Disconnect, source changes and stale input retain release safeguards. Keyboard
control works for display and window sharing. Window mouse and keyboard input pauses
while the shared window is not focused, releases held input, and resumes with the
same permission when focus returns. See [controller support](controller-support.md).

Use Save diagnostic report in the room page. The CLI supports `--report PATH`.
Reports exclude room passwords and membership credentials. Generated tests are
silent and require no mouse/keyboard operation; see
[testing](testing.md).

## Upgrading

Both endpoints must use the modular client and a v2 room. Existing v1 rooms,
keys, NAT invites and direct `--share`/`--watch` commands are not migrated or
silently translated. Create a new room after updating both endpoints.
The v1 Worker is not changed by installing this client.

The current backend is accepted for personal use; remaining hardware/network
qualification is documented in [known limitations](known-limitations.md).
