# Shared-backend room UI

Controller request/grant, selected-peer/all revoke, indicators, selected-device
polling and AppShell panic revoke are now integrated. See [CONTROLLERS.md](CONTROLLERS.md)
for consent/focus/source behavior and silent tests. Mouse/keyboard now use the same
panel with explicit capabilities and exact presented-frame mapping; see DESKTOP-INPUT.md.

The existing home screen now supports guarded v2 routing with
`ScreenShareUi --backend v2 --signal-server HTTPS_ORIGIN`. Start Sharing,
Join Room and Quick Join use the shared backend and pushed directory, within
the current shell. See [ADOPTION.md](ADOPTION.md) for behavior, tests and the
remaining parity/cutover gates. Ordinary launch remains legacy until those pass.

These are explicit integration entry points, not a replacement visual design.
The normal app and both opt-in room entry points now share the existing AppShell.
The room browser and session are pages in that shell, using the current stylesheet
and VideoFrameWidget while exercising the shared v2 backend. Normal create/join
default adoption remains gated; the broader
appearance/usability redesign stays after milestones 1–5.

## Shared application window

`RoomApplication` owns the existing `AppShellWindow`, browser/session pages and
`ScreenAwakeGuard`. Both `--room-v2-browser ORIGIN` and `--room-v2 CONFIG.json`
use it alongside the guarded normal-home route. Browser-to-session navigation preserves
one top-level window, its size and its window state. Leaving a session destroys
its page after asynchronous drain, returns to the browser and opens a fresh pushed
directory subscription. The old page is removed from the stack, so repeated joins
do not accumulate hidden sessions. Config-file sessions close the application when
their page closes; they do not silently inherit browser defaults.

The title-bar close button, Alt+F4 and programmatic shell closure use the same
close handler. Closure disables page actions, requests media/directory shutdown,
and keeps the event loop and window alive until both finish. Repeated close requests
do not repeat shutdown or emit multiple completion callbacks. No nested event loop
or synchronous wait is added. Screen-awake state lasts while the session page is
open and is released on return/exit. Queued page transitions are bound to their
QObject owner. The shell reserves the panic-revoke hotkey only when it has a handler;
v2 still has no remote input support and cannot take the legacy handler's shortcut.

`ScreenShareUiShell` compiles the shared chrome, toast and screen-awake components
once for normal and v2 UI users. The ordinary legacy home/create/join/control
actions remain available in legacy mode pending feature parity and acceptance. This integration
does not complete Stage 2 or enable default v2 cutover.

## Shared audio disabled

The opt-in browser and host session audio selector include **No shared audio**.
Choose it initially or apply it while viewers are connected. Device/process fields
and device refresh are disabled for this selection. On successful handover the old
capture endpoint is stopped and destroyed on its owner thread; the success message
confirms that no audio is being shared. Selecting another source resumes capture,
and a failed resume retains silence. The selection survives capture-worker restart
when viewers leave and rejoin. It is a session setting, not a saved device profile.

The existing silent audio track stays negotiated for live resumption. Viewer mute
and output selection are independent; this option does not close viewer speakers.
Already transmitted audio can drain from receiver buffers after capture stops.

## Presentation recovery

Presentation now checks the native surface **and its root application window**.
A minimized root produces a `minimized` drop even though the child HWND itself is
not iconic. Hidden, invalid and zero-sized targets produce `unavailable`. The
shared UI/CLI backend skips prepare/upload/draw work while blocked, retaining the
healthy device for restore; invalid handles still release it. The low-level NV12
presenter repeats the check before upload and drawing for direct callers and
visibility changes during a frame. No frame retry queue, polling loop or recovery
attempt is added. Existing latest-frame delivery supplies a new frame after restore.

The Windows test explicitly shows its first test-owned window without activation:
the launcher's hidden STARTUPINFO can override Qt's initial show while Qt reports
visible. Startup failures now log native visibility, presentation/drop/error
counters and HRESULT. The injected renderer forwards native outcome diagnostics.
Generated WGC tests validate even, centered pillarboxing for their taller window;
only the synthetic 16:9 source is expected to fill the complete 16:9 output.

UI and CLI now use `backend/render/FramePresentationBackend` and
`FramePresentationSession`; recovery is in `backend/render/PresentationRecovery.h`.
The frontend owns only Qt integration, the existing one-slot worker queue and
presentation timing counters. Backend construction/destruction stay on that worker.
The native renderer receives a synchronous borrowed NV12 view while the queued
frame retains the pixel owner. No additional pixel copy or queue is introduced.

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

The canonical nickname is persisted via QSettings' user-scoped INI profile
(`ScreenShare/RoomV2Profile`). Validation uses the same normalization, length and
control/bidirectional-character rules as the wire protocol. New or invalid stored
names receive a saved `Guest-xxxxxxxx` name from the system RNG; existing valid
names stay unchanged. Nicknames are display labels, not proof of identity. Passwords,
room IDs, service URLs and membership tokens are not saved by this profile; the
password field clears after launching a session. Room titles are rendered as plain
table text. Saved nickname changes apply to future admissions; the session's live
nickname control changes only the current membership.

Browser-launched sessions also offer **Save stream settings for new rooms** (host)
or **Save playback settings for new sessions** (viewer). These persist the current
draft for future sessions; **Apply** remains the separate action for active media.
The saved stream group contains preset, resolution/FPS/bitrate modes, dimensions,
limits and optional aggregate allowance. The playback group contains volume/mute.
Capture handles, process IDs and device identifiers are deliberately not profile
defaults. New browser sessions load these groups before constructing the runtime.
Standalone JSON UI/CLI configurations stay authoritative and do not implicitly load
or overwrite profile defaults.

Versioned JSON groups use strict validation shared with CLI stream configuration.
Unknown fields, invalid types/ranges and malformed/oversized blobs fall back to
safe defaults for the affected group, without resetting the nickname or other group.
Invalid save attempts preserve old values; write failures are reported and restore
the previous in-memory key. Saving valid stream defaults does not claim hardware
support or remote application; normal runtime apply/error reporting still applies.

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

Controller and mouse/keyboard consent/control are integrated into this session
page, including source mapping and confinement. The opt-in browser supplies create/join, persisted
nickname and pushed directory updates inside the normal shell. Live audio-device
switching and retained GPU presentation are implemented. Physical-device and
network/latency acceptance remain open.
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

Actual-widget tests switch to device-free None capture, check decoded Opus
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
accepted a block, not that a speaker physically played it. Healthy output retains
its device pacing, without an extra queue, room mutation, renegotiation or service
request. Settings survive an ADM
playout stop/restart; the independent playback revision reports application.

Stop cancels the public command and drains native work. Endpoint writes honor the
stop token, but synchronous Windows device initialization cannot be forcibly
interrupted; driver hangs and physical unplug/recovery still require acceptance.
Tests use synthetic outputs exclusively, including the Windows widget variant.

## Audio failure and retry

Separate capture/output health labels distinguish failed devices from intentional
None capture. A reported startup/live error releases the endpoint and continues
video with paced silence/discard. The existing action becomes **Retry selected
audio** or **Retry playback**; selecting a different endpoint is also supported.
Retrying the same endpoint reopens it, commits only after PCM/write success, and
retains volume/mute. No automatic retry, device fallback or rejoin is introduced.
See [AUDIO-RECOVERY.md](AUDIO-RECOVERY.md) for state, ownership and physical-test limits.

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
# Per-viewer sender diagnostics (2026-09-17)

The dedicated PeerDiagnosticsWidget now provides payload/FPS and WebRTC limiting
reason columns plus a live nonmodal Viewer details popup. It stays pinned to the
chosen peer and clears after departure; room controls remain usable. Details show
selected-path RTT, bandwidth estimate, linked RTCP loss/jitter and receiver age,
with explicit units and unknown values. Duplicate member names show peer IDs;
capacity above four shows a performance-cost warning. See NETWORK-DIAGNOSTICS.md.

Host rows now include a separate Receiver decoded column. Inline details show
receiver-reported decoded frame count and optional FPS, with unknown/stale values
instead of fabricated zero. Reports expire after three seconds independently of
source/sender observations. See [RECEIVER-TELEMETRY.md](RECEIVER-TELEMETRY.md).

Viewer sessions also display local presentation diagnostics once per second:
presented/dropped/pending counts, the last frame-attempt outcome, reason counters,
graphics error code and recovery budget usage. The renderer snapshot uses a short
dedicated lock, never held during GPU work. These are local counters, not remote
telemetry or end-to-end latency measurements. A terminal failure still leaves
audio and room controls available.

The opt-in host window lists each viewer with its peer identity, application state,
last source-observed dimensions, applied video cap and measured transport upload.
Select a row for requested preferences, revision details and allocation. These
inline details retain selection by peer ID and clear it when that peer leaves.
Unchanged snapshots do not rebuild the table. Nicknames render as plain text.

Transport samples come from the existing local 1 Hz collector, expire at three
seconds, and display fresh zero separately from unknown/stale. Transport includes
audio/protocol traffic but excludes IP/interface overhead. Source observation is
not remote presentation; remote latency and congestion reasons remain unknown.
No additional service requests or telemetry uploads are introduced.
