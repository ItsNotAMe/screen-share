# ScreenShare usage

The modular room backend is the default. Open `ScreenShareUi.exe` to create or
join a room. No backend switch is needed. The default service is the isolated v2
Worker with a ten-room cap; `--signal-server HTTPS_ORIGIN` selects another
compatible deployment.

Normal desktop launches remember the window's size, position and maximized state.
Fullscreen viewing and minimization are temporary. Off-screen saved placement is
adjusted for the available display using [Qt's geometry restoration](https://doc.qt.io/qt-6.10/restoring-geometry.html).
Invalid layout settings fall back to the normal startup layout.

When the host ends a room, viewers return Home with a brief explanation. A room
that expires after the host disconnects or closes with the service has its own
notice. Leaving a room yourself stays quiet.

Join errors explain whether a room is full, closed or missing, or requests were
rate limited. Your invite stays in the form so you can correct it or retry.
An unconfirmed outcome asks you to wait before trying again. Room creation
distinguishes a full service from a room's viewer limit.
Session windows retain these specific failure explanations through status
refreshes. If an active room loses its service connection or its audio/video
fails, the message explains the failure and how the host or viewer can retry.

While joining, the form shows progress and offers **Cancel** in place of Paste.
Cancel preserves the invite so you can edit it or try again. Choosing Home also
cancels the pending attempt and waits for cleanup before returning. The invite
and other room actions stay locked until that attempt finishes.

Create checks the room name and password before starting a session, keeps invalid
input for editing and focuses the field that needs attention. Room names allow
1–64 Unicode characters after normalization and trimming. Long passwords may
need fewer non-English letters or symbols to fit the service limit. The Join
password dialog explains invalid input in place without restarting the attempt.

Home and Join show each room's viewer count and limit (for example, **2 / 4**).
Full rooms show **Full**; a reconnecting host or unavailable directory shows
**Wait…**. Hover over the disabled action for the reason. The action returns to
**Join** automatically when the room list confirms that joining is available.

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

The source picker in Create room and Change source can search display/window
titles. Search is case insensitive and stays in place when changing tabs or
refreshing. Clear search restores the available sources. With no matches, sharing
is disabled; Enter from search moves to the matching cards without sharing.
The picker uses three columns at normal widths and two at compact widths so
more choices fit while previews and titles remain readable.

See [CLI configuration and options](cli.md) for complete schemas and ranges.
Shared source audio is the default: window sharing captures the selected app's
audio and its child processes; display sharing captures system audio. It follows
source changes and mute/unmute. Windows isolates app processes rather than
individual windows, so other windows or tabs in the same app may be audible.
For apps that render sound in helpers (including Miyomu's WebView2 player),
shared source audio follows the app's render sessions and targets their smallest
common process subtree. It refreshes as playback starts or helpers restart and
does not include sessions belonging to another app.
Shared-source capture retries temporary startup/read failures automatically,
with delays capped at five seconds. Video continues during recovery. An
unavailable app stays silent; recovery never substitutes unrelated system audio.
Changing the selected source bypasses the previous source's retry delay, and
muting or stopping ends recovery for the old source.
App audio needs Windows build 20348 or later (including Windows 11). If capture
fails, video continues with silence. Choose System audio to share all apps instead.

Host system audio and microphone capture follow Windows' default device when
no specific device is selected. Changing the default speakers or microphone
reroutes capture during the room without rejoining or changing the audio
selection. A specific device stays selected, and app capture stays with its
selected process. If opening the replacement fails, shared-source mode retries
automatically; explicit System audio or Microphone mode uses the existing retry
action. Video continues during the interruption.

Hosts can mute audio or choose a different audio source while a room is empty.
Mute audio applies to current and future viewers; Share audio restores the last
source. While muted, the host summary shows Audio muted. An idle source selection
is saved without opening an audio device. Capture starts with that selection when
a viewer connects; an unavailable source stays silent and reports the failure.

With current clients at both ends, viewers see an explanation when the host
pauses video, minimizes the shared window, loses the source or recovers capture.
The old picture clears during these states. A resuming message remains until a
new frame is presented. Audio and room controls remain available.
Held keys and buttons are released, and remote input pauses until fresh video
arrives. Host permissions are kept. Click the video again to recapture Game mouse.
The Controls hint explains when remote control is waiting for video or paused
after a focus/mode change. Click the video to resume a focus/mode pause; the
video-wait hint clears after fresh video arrives. Shortcut reminders stay visible.

The desktop viewer also pauses remote control after two seconds without a fresh
presented frame, even while the room remains connected. Held mouse buttons and
keys are released, the controller sends neutral input, and the video area shows
**Video interrupted**. Mouse/keyboard/controller permissions remain granted.
Fresh presentation restores availability without rejoining; Game mouse needs a
new click to capture again. Audio and room controls can continue during the gap.
Queued/decoded frames and redraws of an old picture do not count as fresh video.

Viewer playback follows Windows' default output when **Default** is selected,
including switching to a new default headset or speakers during a room. If the
output is temporarily unavailable, playback retries automatically while video
and the room continue. Volume and mute are preserved. An explicitly selected
device stays selected and is retried without substituting another output. You
can still choose a different device or use **Retry playback** immediately.

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

Hosts can choose the **Remove viewer** icon (a person with a minus) beside the
mouse, keyboard and controller icons on a viewer's row, then confirm the name to
disconnect that viewer's video, audio and remote control. Other viewers stay
connected. Cancel keeps the viewer in the room. The removed viewer returns to
Join with an explanation. Removal ends the current membership; it is not a ban,
and the person can join again with the room link and any required password.

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
