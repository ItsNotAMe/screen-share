# Opt-in shared-backend room UI

## Room browser and saved nickname

`ScreenShareUi --room-v2-browser https://your-service.example` opens the v2 browser
without a configuration file. It offers a saved nickname, display/window capture
selection, system/microphone audio, public/unlisted room creation, an optional room
password, a pushed public list, and direct join by room ID. Choose a room and enter
its password when required, then join. Admission is one operation; the frontend
does not issue a separate availability/password preflight.

The directory opens one WebSocket while the browser is visible. It receives
validated snapshots/deltas and keeps generation/revision ordering. Hiding the
browser for a session stops the subscription; returning obtains a fresh snapshot.
Stale lists cannot be used for selected-room joins during recovery. Direct join
still goes through authoritative admission. Reconnect backoff/heartbeat are the
shared transport's; automatic reconnects are capped at three per rolling minute,
then the user can retry explicitly. The 25 ms control timer drains a bounded local
event queue and generates no room-list HTTP polling.

Only the canonical nickname is persisted via QSettings' user-scoped INI profile
(`ScreenShare/RoomV2Profile`). Validation uses the same normalization, length and
control/bidirectional-character rules as the wire protocol. Invalid stored names
fall back to Guest. Nicknames are display labels, not proof of identity. Passwords,
room IDs, service URLs and membership tokens are not saved by this profile; the
password field clears after launching a session. Room titles are rendered as plain
table text. Nickname changes apply to future admissions; live profile mutation is
still outstanding.

Closing a session returns to the browser after media/network drain. Stop alone
leaves the session window available for inspection; close it to return. Closing
the browser waits for any active session and directory stop before exiting. The
browser uses the current style; a broader visual/usability redesign is separately
planned after the existing refactor milestones.

## Configuration-driven sessions

Run `ScreenShareUi --room-v2 CONFIG.json` with the same configuration documented in
[ROOM-CLI.md](ROOM-CLI.md). The shared parser validates service origin, membership,
capture/audio choice and stream preferences. Production requires HTTPS and has no
plaintext override. Existing startup without this argument still opens the normal
UI; the default backend has not changed.

The new session window uses the existing application stylesheet and video widget.
It shows the assigned room ID (selectable for copying), connection/recovery state,
peer counts and errors. Hosts can apply Gaming/Quality, Auto/Fixed/Native canvas,
Auto/Manual FPS and bitrate preferences while viewers are connected. An optional
Auto bitrate limit is retained when changing other settings. Manual bitrate always
requires a limit, without forcing a minimum rate or padding. Capture/audio selection
comes from the startup configuration; switching devices during a session remains
outstanding.

Settings show pending, applied or rejected counts. Applied means the local sender
accepted the preferences and its source processed a frame at that revision; it is
not a remote presentation acknowledgement. Repeated edits before dispatch replace
the pending value. At most one update is in flight plus one latest pending value.
Invalid edits report an error without altering the active stream. Joining viewers
inherit accepted preferences. Timed `changes` and `seconds` also work in this UI;
after timed stop the window remains available to inspect the final status.

Stop begins asynchronous network/media drain. Closing a running window waits for
that drain while Qt continues processing events, then closes the window. Frame
callbacks publish to the shared latest-frame sink; the UI consumes at most one
frame per timer tick and passes NV12 to the existing renderer. Source frames and
runtime references are released after stop. Normal lifecycle does not block the
GUI thread waiting for capture retirement. Destruction is a final synchronous
fallback; owners should use stop/finished before destruction.

Remote control is unavailable in this opt-in window; no input handler or control
grant is installed. The opt-in browser above now supplies create/join, persisted
nickname and pushed directory updates. The default application shell, live profile/
policy mutations, room links and consent/input controls remain to migrate.
Source switching, aggregate bandwidth allocation, zero-copy
presentation, NAT/ICE configuration and acceptance measurements also remain.
This path is an integration preview, not milestone 2 completion or default cutover.

## Headless and Windows checks

`room-v2-qt-ui` runs the actual Qt widgets offscreen against the isolated Worker,
with synthetic capture/audio. Programmatic spinbox/button/close actions require
no physical input. It tests host/join, frame-size changes, invalid settings, a burst
of coalesced edits, viewer close-after-drain, host stop, owner reuse, and a held
native shutdown barrier while Qt heartbeat timers continue firing.
It also exercises browser create/join, password failure and recovery, media after
joining, pushed counts/removal, nickname normalization/persistence, plain room
titles, password clearing, hidden-subscription shutdown and rapid hide/show.
An independent subscriber verifies updates while both browser windows are hidden.

```powershell
ctest --test-dir build/sdk-app-release -R "^room-v2-qt-ui$" --output-on-failure
```

The explicit Windows variant uses a generated capture window and the same session
widgets with the real renderer. It additionally checks presented-frame counts.
Audio remains synthetic and silent; no keyboard/mouse input is injected.

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-app-release/RoomUiWindowsTests.exe build/webrtc/room-ui-windows windows-media
```

Run the Windows variant in a graphical session outside the capture-restricted
sandbox when necessary. Offscreen results do not prove on-screen presentation;
the Windows proof still does not measure physical-display latency or prove
hardware-only encoding. Existing security, resource, gaming and service-cost gates
remain in [TODO.md](TODO.md).
