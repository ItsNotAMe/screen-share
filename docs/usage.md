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
For apps that render sound in helpers (including Miyomu's WebView2 player),
shared source audio follows the app's render sessions and targets their smallest
common process subtree. It refreshes as playback starts or helpers restart and
does not include sessions belonging to another app.
App audio needs Windows build 20348 or later (including Windows 11). If capture
fails, video continues with silence. Choose System audio to share all apps instead.

Shared audio negotiates stereo between current versions. At 100% viewer volume,
the app applies no playback attenuation. System capture from 5.1/7.1 outputs
preserves front-channel levels, mixes center/surround at -3 dB and omits LFE.
Only mixes that would clip are reduced to fit PCM16. Windows mixer levels and
output-device settings still affect the loudness heard by the viewer.

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

For 3D games, update both computers, share the game window, and grant Mouse and
Keyboard. Granting permission leaves the host's current window focused; the host
then selects the game when ready to play. On the viewer, turn on the **Game mouse** toggle and
click the video. The first click captures the mouse without clicking in the game.
Movement uses raw relative counts, with the hidden viewer cursor locked at the video centre, so camera
turning continues at screen edges. Clicks and wheel input do not reposition the
host cursor. Disable Game mouse for ordinary desktop pointing.

With keyboard control active over the video, Escape opens the game's menu and
Tab reaches the game. **Ctrl+Alt+Shift+F** toggles viewer fullscreen;
**Ctrl+Alt+Shift+Q** releases viewer control and unlocks the mouse. Focus loss,
permission removal, source changes and closing the viewer also unlock it and
release held input. After a focus change, click the video to capture again.
The host's global panic shortcut remains Ctrl+Alt+Shift+F12. In Game mouse mode,
local and remote mouse input can both reach the game; the desktop mouse cooldown
is bypassed because a game's cursor recentering can trigger it.
The Controls panel always shows the shortcuts. Entering fullscreen or capturing
the game mouse also shows a brief reminder over the video without stealing focus.

Game mouse can use a verified window identity even when capture pixels differ
from desktop window bounds; absolute pointing still needs exact bounds. Input
uses Windows SendInput. A game running at a higher integrity level can reject it,
and games that require hardware input may need controller support or a different
input backend. Native regressions do not establish Stellar Blade compatibility.

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
