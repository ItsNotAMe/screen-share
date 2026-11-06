# Testing

Diagnostic coverage and report collection are described in [diagnostics](diagnostics.md).
`room-diagnostic-report` checks older decoder interface compatibility, bounded
history, default ICE configuration, cross-report correlation and redaction of
synthetic transport secrets. `room-v2-input-media` includes a real ICE restart
while other viewers continue streaming. These do not qualify Windows 7 or WAN NATs.

`update-session-deferral` exercises a verified local download while a room is
active, readiness after leaving, the new-room-before-click race, and closing
the update UI during a transfer. `room-v2-qt-ui` checks the shell's session
guard with the real room application lifecycle.

`WindowPlacementChecks.h` runs in both the room UI suite and the native
`WindowChromeTests` executable. It reopens normal/maximized layouts, closes from
fullscreen and minimized states, preserves the first-close snapshot during a
simulated asynchronous drain, rejects invalid/oversized settings and relocates
synthetic off-screen placement. Normal Home also reopens its actual profile to
verify startup persistence. Every check uses a temporary INI file.
Native chrome/placement checks pass at `QT_SCALE_FACTOR=1` and `1.5` with
`QT_QPA_PLATFORM=windows`. They use their own windows and inject no desktop input;
off-screen geometry simulates a missing monitor, without physically unplugging one.

Build tests with the same native toolchain as the application; see [build](build.md).

`capture-source-failure` injects acquisition failure, failed rebuild, exhausted
three-rebuild recovery and first-frame timeout into a replaceable capture source.
It requires source retirement/destruction on its owner, persistent failure reason,
no automatic reactivation or stale delivery, failed replacement handling,
successful replacement with monotonic sequence/generation, a new bounded recovery
budget and cancellation of pending replacement on Stop. Ordinary sources, initial
Start failures and consumer errors retain terminal behavior.
An in-progress replacement must survive failure of the original source and
commit when its first frame arrives.
The four-viewer `room-v2-input-media` proof requires continued audio, released held
input, rejected grants during the outage, unchanged room/peer identities and
connection generations, then resumed video after explicit source replacement.
The focused UI `source-switch` scenario checks two viewers, cleared failure
messages on recovery, available Change source and disabled grant actions during
failure. Set `SCREENSHARE_UI_PREVIEWS` to save host/viewer and compact host renders.
These use injected capture failures, without resetting a physical graphics driver.

`decoder-adapter-lifecycle` first checks caller-owned output-buffer reuse with
changing pixels across 320×180, 640×360, 1920×1080 and back to 320×180. It requires
bounded allocations per format, visible cropping and timestamp association,
unchanged retained frames after later decoding/Stop, released scratch storage
on Stop and an empty cache/counter on restart.
`DecoderComputeBenchmark` is an explicit performance tool, outside CTest; see
[the matched decoder comparison](performance.md#reused-software-decoder-output-storage-2026-11-06).

`decoder-adapter-lifecycle` also runs real MF software decoding and injected
unavailable-GPU startup fallback. A valid encoded picture with an undersized
dimension declaration triggers deterministic output-validation failure. Five
outages separated by at least 30 valid output frames must recover without
reconfiguration; short bursts between failures must still exhaust the three-
restart limit. The recovery proof counts actual output and tolerates bounded
buffered delivery, discarding outstanding RTP expectations on failure. Checks
retain the 250 ms backoff and keyframe requirement, RTP/NTP association, immediate
output, visible cropping, dynamic resize and repeated release/configure cycles.
`DecoderAdapterTests --gpu` additionally exercises generated-input hardware
retirement, software fallback, malformed-input exhaustion and retained textures.
Neither mode captures the desktop or qualifies the reported laptop driver issue.

Report export checks use Qt's nonnative file picker for deterministic automation.
They cover configured/suggested filenames, a chosen destination, JSON suffix,
cancellation, remembered paths, duplicate actions and session-close dismissal
for both roles. The main room UI flow requires video frames to continue while
the picker is open. An injected selected-path race covers write failure and
retry; redaction and independent log-folder access remain checked. Native
Windows picker interactions are not exercised by the offscreen proof.
Populated-folder renders and palette checks cover the fallback picker's file
views, sidebar and selection contrast, with themed navigation and scrollbars.

Room UI admission feedback uses a local HTTP fixture to cover full, closed,
missing and rate-limited rooms, plus an ambiguous server failure. It checks typed
session errors, preserved invites, retry availability, no password prompt for
these failures, and no media runtime startup. A standalone host checks service
capacity guidance. Run it alone with `RoomUiTests.exe http://127.0.0.1:1
admission-feedback` and `QT_QPA_PLATFORM=offscreen`; no external service is used.

The same fixture holds admission responses to check joining progress, Cancel,
Home-after-cleanup, preserved invites and blocked duplicate/other-room actions.
A queued success status after cancellation cannot reopen the session. It also
checks stable control/list geometry through pending/cancelled states and that
cancelled requests start no media runtime.

Form preflight checks reject blank/overlong/direction-control room names and
overlong or control-character passwords without starting a session or sending
an admission request. They retain drafts and focus the offending field. Valid
64-character Unicode names and 128-byte non-English passwords reach the local
HTTP fixture unchanged. Join password correction stays in the existing popup;
compact/normal feedback renders are captured. Backend admission and UI password
checks share the same validator; wire normalization remains authoritative.
Create resize checks require full-height fields inside both their card and the
visible scroll viewport, so sibling cards cannot cover the focused password.

The admission fixture also opens actual host/viewer session windows and checks
that capacity guidance survives terminal status refreshes. Synthetic active-room
failure pushes check media/transport guidance, host/viewer recovery actions, and
replacement of stale action errors. These message-policy checks do not inject a
physical device or WAN failure.

Room UI closure checks stop a real local-service host and require the viewer to
return Home with a room-ended notice after cleanup. Voluntary Leave stays quiet
and keeps the host active. Removal retains its separate explanation. Policy
checks cover expiry/service-closure notices and quiet host/local exits; report
checks cover every bounded closure reason and the absent-reason case.

Source-picker checks use synthetic titles and diagnostic selections without
desktop enumeration. They cover filtering, selected-card identity, thumbnail
updates, no-match versus unavailable states, explicit Clear search, query retention
across tabs/refresh, disabled sharing and Enter/focus behavior. The room UI also
checks that the live host's search/audio/action controls fit the compact and
normal popup layouts. Source-search checks run in the default room UI flow and
the focused `admission-feedback` mode.
Grid checks require three columns and six fully visible choices at normal size,
two columns at compact size, preserved selection and stable wrapping while
scrollbars appear or disappear.

Ordinary tests use generated media and injected input sinks. Keep physical
input, driver allocation and audible-output tests explicit.

`encoder-adapter-lifecycle` includes four simultaneous software encoders fed
eight changing NV12 buffers with unequal padded plane strides. All 32 outputs
are decoded to validate luma/chroma, dimensions, timestamps and input mapping;
instrumented buffers must receive no ToI420 calls. Existing cases also cover
rate changes, zero-rate suspension, keyframes, coalescing and repeated release.
The completed-stall regression delays software input mapping for 550 ms three
times through the real MF encoder. Late pictures must be dropped without codec
reinitialization or terminal failure. Fresh input must resume automatically with
its original RTP/NTP and input mapping; a new real decoder must decode each
recovery keyframe independently. Pending input still coalesces to the latest
frame, with exact drop accounting. Release during a fourth stall suppresses old
callbacks, and a new configuration starts cleanly. This injects mapping delay;
it does not reproduce a physical GPU hang or preempt a stuck native call.
The direct-plane entry point rejects truncated planes, invalid strides and
dimensions, and use before startup/after Stop; rejected inputs must not consume
the first frame's timestamp. Minimal final rows need no trailing padding.
Run `EncoderAdapterTests.exe --gpu-readback` explicitly for generated GPU input:
four encoders share one readback per source frame without I420 conversion. It
checks CPU preview materialization, both CPU format request orders, cached
ownership and rejection of new mappings after device retirement. No desktop
capture, physical input or audible output is used by this test.

`EncoderComputeBenchmark` is an explicit timing tool, excluded from CTest. It
warms codecs before measuring aggregate process CPU and elapsed workload time,
checks exact delivered-frame counts and no drops, forces fresh GPU readback
wrappers per input frame, and hashes encoded output. See [performance](performance.md)
for the paired-run command, results and limitations.

`video-frame-input` also checks five isolated renderer outages separated by 30
successful incoming presents. Recovery totals remain cumulative. Twenty-nine
valid frames between losses still exhaust the three-rebuild budget; old-frame
redraws, busy/occluded/minimized/unavailable output and backoff cannot renew it.
Terminal failure stays terminal until Clear, and nonrecoverable errors retain
their behavior. Supplied time keeps the policy boundary deterministic; the shared
UI/CLI session separately checks redraw isolation with an injected backend.
The actual widget worker also recovers five healthy-separated outages with an
injected renderer, preserving cumulative totals and the one-frame queue setting.
`RoomUiWindowsTests` additionally runs four healthy-separated injected losses
through the actual native presentation worker and GPU resource recreation,
retaining the one-frame queue and zero readback. It injects an HRESULT after GPU
work, without resetting the physical graphics driver.

`video-frame-input` checks the two-second presentation freshness boundary with
supplied timestamps, verifies that resize/redraw and skipped presents cannot
refresh old video, and holds a render across Clear to reject late input mapping
and freshness updates. The desktop-input UI scenario suppresses only the video
consumer while real transport/decoding continue, checks held mouse/key release,
blocked input, retained permissions and automatic recovery, and can save compact
and normal interrupted-video renders. The viewer-removal scenario additionally
checks controller neutralization and recovery under its existing grant during
that consumer stall. Synthetic input UI proofs inject a successful presenter:
offscreen Qt HWNDs can be unavailable to DXGI. Explicit Windows UI proofs retain
the native renderer. These injections do not reproduce a particular WAN outage.

`stream-settings-and-adaptation` checks independent viewer preferences and frame
mapping using CPU buffers. Run `StreamSettingsTests.exe --gpu` explicitly for
generated-texture GPU coverage, without desktop capture or audible output. Its
four-viewer comparison uses the same 24 generated 1080p frames and 720p settings:
separate input wrappers reproduce independent scaling, while one shared input
exercises production fan-out. It counts actual scaling submissions, CPU
readbacks and distinct output textures, validates changing pixels and independent
preset metadata, and retains old output to detect overwritten resources.
It also verifies that image rectangles produce distinct cached output, the fifth
geometry evicts the oldest of four entries without invalidating consumers, and
retired devices cannot return cached output. The existing GPU settings scenarios
cover independent adaptation, fallback/quarantine and restricted-thread teardown.

`source-pcm-capture` uses an injected clock to check shared-source recovery with
250 ms–5 s capped backoff, same-target reactivation, immediate source changes,
fresh PCM, cumulative drop counters and cancellation during startup. Invalid
targets remain silent and process failures never fall back to system capture.
The four-viewer public-session proof uses the real shared-source wrapper around
synthetic endpoints: capture resumes without a source-change command, audio/
room revisions remain stable, and muting during a second outage leaves no live
capture endpoint or later retry. Native driver failure/recovery is not injected.

Default host capture routing is covered in `native-pcm-audio` with injected
endpoint notifications. Tests exercise changes before/during reads and
activation, coalesced events, silence before fresh PCM, cumulative drops,
unrelated failures, cancellation, and the existing shared-source retry policy.
Callback checks filter render/capture flows and communications roles. Factory
checks verify that default system/microphone capture follows routing while
explicit device IDs and process capture remain isolated, without opening native
devices. The four-viewer public-session proof injects a live default capture
change and requires audio/video progress, one live capture, stable room/audio
revision and no failure; a muted route change must not activate capture. These
tests do not qualify physical device switching or driver activation failures.

Automatic host System/Microphone recovery is covered in `native-pcm-audio` by
`CaptureAudioRecoveryTest.h`: supplied time checks 250 ms–5 s capped retries,
repeated startup/read failures, pinned selections, fresh PCM, unchanged settings
revision, cumulative drops and microphone-processor recreation. It checks manual
Retry during backoff, mute/source replacement during blocked activation, the
five-second first-PCM deadline, Close/stop and owner-thread teardown. Explicit
process IDs and shared-source mode cannot enter this second retry mechanism.
The source-switch UI proof requires host capture to recover without clicking
Retry while video continues, retaining the audio selection/revision. Host failure
messages are rendered at compact/normal sizes for shared, system and microphone
modes; explicit process mode keeps manual guidance. These use injected devices
and synthetic media, without native capture, audible output or physical hot-plug.

Automatic output recovery is covered in `native-pcm-audio` with an injected
clock/device. It checks capped 250 ms–5 s backoff, repeat outages, fresh PCM after
activation, mute/volume retention, unchanged settings revision, manual override,
pinned-device isolation, failed first writes and Close/cancellation races.
Default-output callback tests filter capture/communications roles, retain
loss/arrival notifications and exercise activation races and event coalescing.
The local-service source-switch UI scenario requires audio to resume without a
Retry click while video and room identity continue. These are injected endpoint
and notification tests; they do not qualify physical headset unplug/replug or
all audio drivers.
The four-viewer public-session proof reroutes one default output and requires
continued media for that viewer and the others, with stable room identity and
playback revision. The UI also keeps output/volume/mute controls reachable during
failure: Retry must occupy its form row instead of overlaying other fields, and
its row disappears after automatic recovery.

`room-status-cadence` uses the local Worker and an instrumented four-viewer
runtime to require about ten diagnostic snapshots per second while media/control
advance independently. Room UI checks also cover access-badge/icon changes,
keyboard focus retention, no-match search recovery and stale directory rows.
Directory availability checks cover occupied/total capacity, full/reconnecting
reasons, disabled actions, stale-list guidance and restoration after capacity or
connection changes. Synthetic directory snapshots pass through both Home and
Join adapters, with compact/normal renders and action bounds checked. They run
in the default UI flow and focused `admission-feedback` mode; no network fault
or host lease expiry is injected by these message-policy checks.
Room UI audio checks select a source and mute before the first viewer, require
received silence with no host capture endpoint opened, then restore the exact
source on unmute. An outstanding automatic source edit cannot override Mute.
`native-pcm-audio` also covers inactive selection, recording restart, deferred
startup failure without fallback, and rejecting commands after close.
`room-v2-viewer-removal` removes one of two identically named viewers, checks
controller release and rejected stale input, and requires continued media and
the same connection generation for the other viewer. It covers cancellation,
inline icon-only actions fitting their row at compact/normal window sizes,
focus return, departure during confirmation, joining with a new membership, and
the removed viewer's explanation. API checks reject viewer-initiated removal,
host self-removal, invalid/missing targets and overlapping mutations.
Run this scenario alone with `node signaling-worker/tests/run-native-service.mjs
build/release/RoomUiTests.exe build/viewer-removal-evidence media viewer-removal`
and `QT_QPA_PLATFORM=offscreen`.
It uses its own local service so the larger UI run does not exhaust the
production admission rate budget within one minute.

Desktop/controller UI checks require persistent pause guidance on focus loss,
mode changes and host video pause. Video-wait guidance has priority until fresh
video arrives; focus/click recovery clears it without losing grants. Existing
recording sinks still require held-state release and blocked input while paused.
Compact/normal paused-control renders retain the shortcut reminders.

Input-lag regressions stop transport pumping beyond the 300 ms watchdog and
require neutral input with retained grants. `input-service` checks three gaps,
renewed epochs, rejected pre-gap packets, no cached-report/heartbeat replay,
fresh-input recovery, concurrent revoke during blocked Pause and failed Pause
fallback. It also checks fresh queued input during a delayed device call, viewer
withdrawal crossing renewal and ordered release acknowledgements before regrant.
`gamepad-control` verifies the same allocated device/slot remains
usable and an unchanged controller report resumes through a new epoch-bound
poller. `desktop-input` lets geometry expire, requiring released keys and no
stale injection while ownership remains; source invalidation still fails health.
The UI controller fixture checks two gaps, checked grant icons, Input paused
guidance and automatic resumption without host action or reallocation. Its
presentation, device reader and sink are injected. The four-viewer public-room
proof also stalls runtime advancement, preserves grants and resumes transported
input with a fresh epoch. None of these tests reproduces a physical game's lag
or allocates a native virtual controller.

Viewer share-state checks use the production telemetry channel to verify host
pause/resume while audio continues, two-viewer source closure/replacement and
minimize/restore. A Live notice with the UI frame consumer temporarily delayed
must keep the resuming message until a decoded frame arrives. The protocol checks
in `native-room-runtime-link` cover all states, malformed/truncated packets,
connection binding, replay/reordering, receipt expiry and bounded acceptance.
The recording-controller scenario checks neutral input while video is paused
and resumption with the same permission. The recording desktop UI scenario also
holds a mouse button and key across host pause, requiring releases and retaining
the grant before video resumes. These checks inject no physical input.

`NativeCaptureTests --preview` is an explicit active-desktop check: it captures
only its generated window in landscape, portrait and small sizes, checks fitted
preview geometry and reports BGRA readback bytes before/after GPU scaling.

Input service regressions cover accumulating requests, denial preserving existing
grants/held input, requests arriving during grant application and cancellation.
`desktop-input` uses generated HWNDs to check keyboard grants with mismatched or
missing capture-to-desktop bounds. Window mouse grants allow relative game input;
absolute events remain suppressed without exact bounds. Hidden
windows, removed identity markers and changed processes still revoke input.
`NativeCaptureTests --switch` includes a captured borderless window, minimized
keyboard grants and six source replacements; it requires an active desktop and
does not inject keys. These fixtures do not establish Silksong compatibility.
`video-frame-input` checks Qt's native E0-prefixed navigation/modifier scans through
both video event paths, mapping, protocol encoding/decoding and Windows input
payload construction. Dedicated arrows retain the extended flag; keypad scans
remain unprefixed. It does not inject physical input or qualify a specific game.
Game-input regressions cover relative wire payloads, accumulation instead of
replacement of motion samples, generation isolation, permission removal,
Escape/Tab routing and relative Windows payloads without absolute flags. The
recording desktop UI scenario forwards Escape press/release in fullscreen through
both video recipients, exits using Ctrl+Alt+Shift+F and releases control with
Ctrl+Alt+Shift+Q. These do not establish hardware mouse sampling or game delivery.
`VideoFrameInputTests --game-mouse` with `QT_QPA_PLATFORM=windows` also checks
native registration/confinement at the video centre (including resize), the consumed capture click, relative buttons and
wheel, a hidden cursor across grant updates, and unlocking on permission removal,
focus loss and stream clear. It creates a temporary window, confines the local
cursor briefly and records events without delivering input to a game.

For a two-PC game check, run the same updated build on host and viewer, create a
fresh room and share the game window. Grant Mouse and Keyboard while the host is
in ScreenShare: granting must preserve focus. The host then selects the game;
the viewer enables Game mouse and clicks the video. Check continuous camera
turns past screen edges, combined movement and camera input, buttons/wheel, Tab,
and Escape opening the game menu while viewer fullscreen remains active. Check
Ctrl+Alt+Shift+F for fullscreen and Ctrl+Alt+Shift+Q for release, then request/grant
again. Alt+Tab must release held input and unlock the viewer cursor; click the
video to recapture. Host focus loss pauses game input, and host revoke or
Ctrl+Alt+Shift+F12 must release held keys. Save reports on both PCs for any failure,
including the failed step and game display mode. This real connection check is
separate from recording-sink regressions.
Also leave and rejoin the same room several times, confirming the host stays
active and accepts a new viewer. If the room itself ends, restart sharing on the
host; an ended room cannot be repaired by rejoining from the viewer.
`CaptureSessionTest` in the WebRTC proof suite injects GPU frame deadline misses:
one miss must preserve capture and its generation; three consecutive misses use
bounded recovery, while cancellation and the lifetime retry budget still apply.
Native media reports distinguish `capture-gpu-frame-skipped` from a source error.
The native room UI scenarios check immediate icon requests, independent host menus,
distinct pending/granted states and preserving keyboard when requesting mouse.
Window keyboard regressions check direct grants and accepting requests.
The window keyboard UI fixture constructs the production runtime with window
capture selected at startup, adds keyboard to an existing mouse grant and records
delivery of a key. This covers runtime capability configuration as well as the UI.
Desktop input tests check background input suppression, releasing held keys during idle
health polling, retained ownership and resumption without replaying dropped input.
They also inspect the Windows key payload for arrows, right modifiers, keypad
keys and releases without calling SendInput. `capture-dispatcher-lifecycle`
checks replacement-source queue ownership and external queue borrowing.
The `desktop-input` UI scenario covers visible backend grant failures and checks
that host/viewer media failure sends Leave and promptly updates the room directory.

`room-v2-socket-reconnect` delays the server's socket-close notification and
forces two viewer reconnects plus a host reconnect using the real native room
runtime and local Worker. It checks replacement peer generations, fresh viewer
telemetry, restored control requests/grants/input delivery and continued media.
Viewer reconnects preserve the other viewers' peers. Capture, audio and input
are synthetic; the fault route exists only in the generated loopback fixture.

`room-peer-isolation` uses real native peers with synthetic video/audio. One
viewer receives media while a second completes SDP but has no usable ICE path.
It checks that the unconnected viewer never processes capture frames, the healthy
viewer continues receiving frames through the connection timeout/removal, and
both endpoints retain the timeout evidence after cleanup. It does not establish
real-WAN FPS or Windows 7 compatibility.

`room-peer-departure` exercises the host's real networking thread and signaling
coordinator after a viewer becomes reconnecting or leaves. Queued sends to that
viewer are discarded while another viewer's signaling continues without a room
transport error. The Worker `viewer-departure.test.mjs` covers disconnect, Leave,
kick, late ICE from retired connections, and joining afterward. Run it through
`npm test` in `signaling-worker`; these are local fixtures, not a deployed-service
or WAN qualification.

```powershell
cmake --build build/release --target RoomUiTests RoomUiWindowsTests
ctest --test-dir build/release --output-on-failure
```

CTest runs the registered suite; list it with `ctest --test-dir build/release -N`.
No fixed historical test count represents current coverage. Node.js/local Worker
dependencies are required for room integration tests. UI tests launch their own
isolated local service through `signaling-worker/tests/run-native-service.mjs`.

Focused examples from the repository root:

```powershell
ctest --test-dir build/release -R '^room-v2-qt-ui$' --output-on-failure
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-input-evidence media desktop-input
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-controller-evidence media controllers
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiTests.exe build/ui-resolution-evidence media native-resolution
node signaling-worker/tests/run-native-service.mjs build/release/RoomUiWindowsTests.exe build/ui-native-evidence windows-media
```

The shared source chooser is exercised from both room creation and the host room.
Room UI regressions cover cancel/confirm, keyboard dismissal and focus return,
long source titles, scrolling and popup layouts at several window sizes. The
create form retains the selected-source preview. `SCREENSHARE_SOURCE_PREVIEW_PROOF=1`
opts into actual WGC preview/thumbnail capture with the room UI fixture. Preview
ownership checks include capture frames that already match the thumbnail size.
The source-switch scenario closes the selected source with two viewers connected,
rejects an unavailable replacement, then resumes both viewers without replacing
their connections. The Windows variant closes an actual generated HWND;
`NativeCaptureTests --live` also checks close-and-replace on the capture worker.
To run just the room source lifecycle scenario, use
`node signaling-worker/tests/run-native-service.mjs build/release/RoomUiWindowsTests.exe build/source-close-evidence windows-media source-switch`
(or `RoomUiTests.exe` with `media` for synthetic capture).
Set `SCREENSHARE_SOURCE_SOAK_SECONDS=30` for a steady two-viewer interval before
closing the source. The test accepts 0-40 seconds, checks both viewers' progress
and room health, and records hardware frame/fallback counts in its JSON result.
The generated-window variant exercises actual capture and GPU codecs, with
synthetic audio. It does not reproduce a particular app or remote GPU driver.

The Windows suite needs an interactive desktop and validates capture plus native
D3D presentation. These input scenarios use recording sinks, not real OS input.
Optional `SCREENSHARE_UI_PREVIEWS` saves UI screenshots; use the offscreen suite
for deterministic popup positions. Windows may clamp popup positions to a screen
edge. Qt widget grabs do not include the native D3D video plane: use actual frame
presentation counters and native rendering checks, not a black grab, as evidence.

Audio cadence and retained-frame resize regressions run without physical input:

`av-sync-recovery` runs the production room runtime and encrypted H264/Opus over
loopback with a synthetic output device that stalls repeatedly, then resumes at
device speed. It compares received audio/video NTP timing before and after each
stall, requires continued video delivery, and checks timely recovery. It uses no
physical audio devices. `native-pcm-audio` independently checks that callback work
and wake-up overshoot do not accumulate in the output clock, and that missed
slots do not replay bursts.
These fixtures establish recovery from local playout stalls, not every WAN or
driver failure and not audible fidelity on another PC.

`NativePcmAudioTests --scheduling` is an explicit silent contention check. It
pins synthetic playback and a 750ms higher-priority competitor to one allowed
CPU, leaving the other CPUs available, and requires audio gaps below its 50ms
output reserve. It pins playback after MMCSS registration and checks affinity
so task-policy migration cannot make the comparison pass. It needs two allowed
CPUs and an available Windows multimedia scheduler. Routine CTest does not
create this deliberate CPU load. The A/V recovery test also inspects a live
blocked-write phase and retained timing events before and after recovery.

The PCM audio test checks ordered blocks delivered in 50ms bursts, with no gaps
after startup, and unity-level stereo content in 5.1/7.1 layouts with overload
protection. `SharedAudioLevelTests` sends synthetic audio through the shipping
MediaEngine and encrypted Opus transport at 100% viewer volume. It checks stereo
SDP in both directions, mono codec compatibility, received RMS levels and channel
separation for equal, left-only, right-only and opposite-phase signals. It opens
no physical audio devices. The video/input test forwards six complete key presses
across two busy GPU presents, so renderer frame drops cannot silently discard key events.

`ApplicationAudioTargetTests` checks rendering-helper selection, multiple audio
sessions, unrelated apps, stale/recycled parent PIDs, helper restarts during a PCM
read, playback starting after sharing, and silence after the window owner exits.
Its default run opens no audio devices. On an interactive Windows desktop, run
`build/release/ApplicationAudioTargetTests.exe --app PID` while that app plays
audio to check the actual discovery and shared-source PCM capture path. It prints
the selected process and levels, discards samples, and saves or replays no audio.

```powershell
cmake --build build/release --target NativePcmAudioTests SharedAudioLevelTests VideoFrameInputTests NativeCaptureTests
ctest --test-dir build/release -R '^(native-pcm-audio|shared-audio-level|video-frame-input)$' --output-on-failure
```

On an interactive Windows desktop, `build/release/NativeCaptureTests.exe --cadence`
checks 30/60 FPS on its own stationary test window, including minimize/restore;
it saves no pixels. `build/release/NativePcmAudioTests.exe --wasapi` checks silent
native playback and process-only capture. Neither proves audible signal fidelity
or end-to-end behavior on a second computer.
`build/release/NativeCaptureTests.exe --switch` checks six generated-window source
replacements, continued frames/input geometry, and keyboard grants while minimized
past the normal target freshness timeout. It does not inject keyboard input.

`WindowChromeTests` checks native caption-button hit testing in normal, maximized,
and restored windows. Build that target, then run it on a Windows desktop with
`QT_SCALE_FACTOR` set to `1`, `1.5`, and `2` in separate processes. These overrides
apply only to the test process; they do not change Windows display settings.

The wider runners remain in `scripts/test-headless-media.py`,
`scripts/test-room-regression.py`, `scripts/test-room-hardware-load.py` and
`scripts/test-room-impairment.py`; use `--help` for current fixture/options schemas.
Use the matching evidence validators in `tests` when changing those tools.

`run-webrtc-proof.ps1 -Hardware` opts into GPU tests; `-LiveCapture` opts into
capture fixtures. Its `-AudioDevice` mode requires `-AllowAudibleTests`: never
assume a device test is silent merely because it is unattended.

Physical-reader tests and native virtual-driver allocation are separate opt-ins.
Do not rerun native allocation until the known leftover virtual-device state is
resolved; see [known limitations](known-limitations.md). Test success with recording
sinks does not establish physical controller, HDR, NAT or external A/V latency.

Historical investigation reports are available in Git at commit `376d59a`.
They are not current acceptance checklists. Deferred work remains deferred unless
a reported regression or new request makes it relevant.

## Offline setup regression

`python tests/NativeDependencySetupTests.py` checks bootstrap orchestration with
small mocked installers: no WebRTC downloads, C++ builds, devices or network are
used. It covers cache reuse, separate configurations, interruption recovery,
custom paths, missing/corrupt inputs and CMake's pre-compiler setup. Set
`CMAKE_COMMAND` to native Visual Studio CMake if the PATH points at MSYS.
These checks do not establish that a real dependency download/build succeeded.
