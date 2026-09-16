# Opt-in shared-backend room UI

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
grant is installed. The existing room browser, nickname persistence, profile and
directory updates, consent/input controls and normal create/join forms have not
yet migrated. Source switching, aggregate bandwidth allocation, zero-copy
presentation, NAT/ICE configuration and acceptance measurements also remain.
This path is an integration preview, not milestone 2 completion or default cutover.

## Headless and Windows checks

`room-v2-qt-ui` runs the actual Qt widgets offscreen against the isolated Worker,
with synthetic capture/audio. Programmatic spinbox/button/close actions require
no physical input. It tests host/join, frame-size changes, invalid settings, a burst
of coalesced edits, viewer close-after-drain, host stop, owner reuse, and a held
native shutdown barrier while Qt heartbeat timers continue firing.

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
