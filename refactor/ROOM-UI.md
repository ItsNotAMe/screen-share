# Opt-in shared-backend room UI

These are explicit integration entry points, not a replacement visual design.
The normal app still opens the existing AppShell. The opt-in browser and session
window reuse the current stylesheet and VideoFrameWidget while exercising the
shared v2 backend. Default-shell adoption remains part of milestone 2; the broader
appearance/usability redesign stays after milestones 1–5.

## Presentation recovery

The shared video widget permits three device rebuilds per session, with a 250 ms
drop-only backoff after each recoverable DXGI failure. Successful frames do not
reset the budget. A fourth failure or a nonrecoverable error stops renderer calls;
the v2 window shows an instruction to leave/rejoin while audio and room controls
remain available. An explicit `clearFrame()` establishes a new presentation session
and budget. Closing joins the worker without sleeping through a retry timer.

The existing one-slot handoff remains bounded during recovery. Failed frames are
released, never retried. Renderer construction, reset and destruction all run on
the worker; `FramePresentationFactory` allows silent headless tests of the same
worker. Presentation statistics expose recovery count and terminal state.

Generated-window tests inject typed device loss after real GPU work and verify
resource recreation, exhaustion, explicit clear and the one-frame DXGI limit.
This does not establish recovery from physical driver removal or a hung driver call.

## Room browser and saved nickname

`ScreenShareUi --room-v2-browser https://your-service.example` opens the v2 browser
without a configuration file. It offers a saved nickname, display/window capture
selection, system/microphone audio, public/unlisted room creation, an optional room
password, a pushed public list, and direct join by room ID or v2 link. Choose a room and enter
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
table text. Saved nickname changes apply to future admissions; the session's live
nickname control changes only the current membership.

The session's **Copy room link** button copies `screenshare://room/v2/ROOM_ID`.
Paste it into the browser's **Room ID or v2 link** field. Links contain only the
versioned identifier, never passwords, membership tokens or a service address.
The recipient must use the same configured service and enter any password separately.
Links do not navigate a browser, launch the app through an OS protocol registration,
or switch service origins. Unknown versions, percent-encoded IDs, credentials,
queries and fragments are rejected locally before admission.

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
nickname and pushed directory updates. The default application shell and
consent/input controls remain to migrate.
Live audio-device switching, zero-copy presentation, NAT/ICE configuration and
acceptance measurements also remain.
This path is an integration preview, not milestone 2 completion or default cutover.

## Live display/window switching

The host can use **Refresh capture sources**, select a display/window, and press
**Share selected source**. Source selection is local to the host; viewers cannot
request it over signaling. The portable backend exposes `SwitchCaptureSource` with
one pending operation, typed errors and an independent capture-selection revision.
Room membership, media peers and stream settings remain in place.

The capture owner starts a candidate while continuing to poll the working source.
Only a first valid candidate frame commits the selection; startup failure, closure
or a five-second first-frame timeout discards the candidate. Stop cancels pending
completion and joins capture teardown. Native creation, polling and destruction
stay on the capture worker. Source startup/poll calls must remain bounded; starting
a platform capture may briefly delay acquisition. Completion is first-frame
readiness, not proof of remote presentation, and already queued old frames may drain.

Fixed output settings keep their canvas; Native/Auto can follow source-size changes.
The decoder now accepts bounded keyframe-declared size changes because the pinned
receiver's initial resolution hint may describe only its first frame. Output is
still checked against the declaration and the global 4096-pixel allocation bound.
Windows switches may require GPU readback/copy when the new capture device differs
from the retained encoder device. This is not zero-copy or hardware-only acceptance.
Live audio-device switching is separate and remains outstanding.

## Shared upload allowance

Host stream controls include an optional **Limit total media upload allowance**.
The complete settings edit applies the allowance together with resolution/FPS and
the individual bitrate cap. The same setting is `stream.aggregateUploadBps` in the
shared UI/CLI JSON configuration; valid values are 160,000–1,000,000,000 bits/s.
Omit it or uncheck the control to restore individual caps alone.

Allocation reserves 20% of the allowance for transport/recovery and 128 kbps of
audio per viewer, then divides the remainder equally. Each video cap is the smaller
of that share and the existing individual cap. Negotiating viewers reserve a share
before they send. Joins, departures and settings changes update the allocation;
measured rates never drive a second adaptation controller.

If a share is below 1 kbps, its video encoding is deactivated until the allowance
or membership permits video again. Audio and room membership remain active. Very
small allowances can be below the audio reservation alone: this is an estimated
media budget, not a strict network-interface shaper. Congestion control can send
less, while recovery/probes and overhead can exceed estimates. A rejected sender
update retains its previous cap and is reported as rejected; cross-peer application
is not atomic. Manual resolution/FPS and absence of a bitrate floor are preserved.

The host displays allocated/applied video caps separately from measured WebRTC
transport upload. Native stats are sampled at most once per second per viewer, with
one request in flight. Rates require two compatible counters; resets/replacement
transports invalidate the rate, and samples older than three seconds are omitted.
An aggregate measured value is shown only when every current viewer has a fresh
sample. These counters exclude IP/interface overhead and are not a physical-link
measurement. The CLI emits per-peer `allocatedVideoBps`, `appliedVideoBps` and nullable
`transportSendBps` alongside the selected `aggregateUploadBps` (zero when disabled).

## Live room and nickname edits

Each session displays the authenticated member list. Any member can change their
own session nickname; the host can change room name, public/unlisted visibility and
viewer limit. Session nickname edits do not replace the browser's saved nickname.
Names remain plain text, normalize through the existing protocol boundary and reject
invalid Unicode, control/bidi characters and excessive length. Host authorization
is checked locally and by the server.

The portable backend exposes `UpdateNickname` and `UpdateRoomPolicy`, accepting an
explicit expected room revision from `Status()`. One mutation may be queued or in
flight per session. A result confirms server acknowledgement, not optimistic local
state: member/policy snapshots and directory updates arrive independently. Stream
preferences retain their separate revision and command path.

Drafts retain their starting revision. A conflict preserves the draft and asks the
user to reload current values before reviewing/resubmitting. Nickname and policy
drafts are independent. Controls are disabled during a pending mutation. There is
no mutation retry or extra HTTP refresh. A disconnect, shutdown or ten-second
acknowledgement deadline reports `Unconfirmed`: the server may already have saved
the change. Inspect pushed state before deciding whether to submit again.

The real-Worker widget test covers live normalized nickname updates, stale policy
conflicts, draft preservation/reload, pushed name/capacity/visibility changes,
unauthorized/invalid edits and uninterrupted frames. A signaling barrier tests
the public API's Busy bound and stop ordering deterministically, including resolving
the pending result as Unconfirmed.

`room-v2-mutation-ack-recovery` uses a test-only Worker subclass to delay the first
acknowledgement beyond the production ten-second deadline while still committing
and pushing state normally. It verifies Unconfirmed, no automatic retry, continuing
video, an explicit subsequent mutation, and isolation of the first late reply while
the second request is pending. Production Worker code and timing are unchanged.

## Live shared-audio selection

Hosts can change system-output, microphone or process-output capture with **Share
selected audio**. Refresh enumerates devices only on demand; process capture uses
an explicit process ID. The selection requires active recording (normally a connected
viewer). It changes the shared outbound audio, not the viewer's playback device.
Host source/settings controls scroll so Stop remains outside the long form.

The public `SwitchAudioSource` operation accepts one pending request. A replacement
starts on its own capture worker while the old source continues; its first PCM block
commits the independent audio revision. Invalid/startup/first-block-timeout errors
retain the previous healthy source. Stop cancels pending work and joins endpoint
owners. Successful selection survives a recording stop/restart. No room mutation,
admission, renegotiation or periodic service request is added.

Each producer retains one 10ms block; the WASAPI packet adapter retains at most
20ms, keeping the combined application capture handoff at 30ms. The consumer follows
the source cadence, substitutes silence for a missing block and discards stale
handoff data. Process-loopback activation honors cancellation; its completion event
is owned by the async handler so a late completion cannot signal a reused handle.
Native driver calls cannot be forcibly preempted by the adapter.

Actual-widget tests switch to silent synthetic microphone PCM, check decoded Opus
becomes silent while video continues, reject a missing device without committing,
then restore decoded audio. Viewer requests are rejected and room/member/settings
revisions remain unchanged. These tests never capture or play physical audio.
Physical device switching/unplug/recovery remain open acceptance work; synthetic
results do not establish them.

## Viewer playback controls

Viewers can refresh output devices, select an output, set volume (0–100%) and mute,
then apply the settings. These controls affect only that viewer. Host capture and
other viewers are unchanged, and video continues while playback is muted. An empty
device selection uses the default output. Device enumeration runs only on request.

`RoomSession.UpdatePlayback` accepts one pending command while playback is active.
The existing ADM playout worker owns endpoint creation, first write, replacement
and destruction. Volume/mute changes reuse the device; changing the device can
temporarily pause local sound during initialization. The old endpoint/settings
remain available on startup or first-write failure. Success means the new endpoint
accepted a block, not that a speaker physically played it. No queue, clock, room
mutation, renegotiation or service request is added. Settings survive an ADM
playout stop/restart; the independent playback revision reports application.

Stop cancels the public command and drains native work. Endpoint writes honor the
stop token, but synchronous Windows device initialization cannot be forcibly
interrupted; driver hangs and physical unplug/recovery still require acceptance.
Tests use synthetic outputs exclusively, including the Windows widget variant.

## Retained NV12 presentation

The shared frame sink now hands packed decoded NV12 to the UI with its immutable
buffer owner attached. It does not convert NV12 to I420 and back or copy pixels
into a second UI vector. Padded NV12 gets one explicit plane pack; non-NV12 input
uses an explicit conversion fallback. Visible dimensions and timestamps survive
the handoff. A stopped sink releases its pending frame and rejects late callbacks;
every session gets a fresh sink. The existing renderer queue remains latest-only.

The v2 window enables nonblocking presentation and measures a DXGI maximum frame
latency of one. Busy/occluded frames are discarded, and presentation counters only
count successful presents. `QtRoomSession.frameStatistics()` reports received,
replaced, delivered, directly retained, converted and repacked frames. Widget
statistics expose actual queue configuration and presentation errors. These are
local diagnostics, not physical display-latency measurements.

The current MF decoder still produces CPU NV12 and the renderer still uploads it
to D3D textures. This removes the redundant CPU handoff work; it is not hardware
decoding or GPU zero-copy presentation. Those acceptance items remain open.

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
