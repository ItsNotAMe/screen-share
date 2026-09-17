# Opt-in v2 room CLI

Host `"audio": {"source": "none"}` starts without opening an audio capture device.
The same `source: "none"` works in timed `audioChanges`; a subsequent system,
microphone or process change resumes capture. None rejects nonempty `deviceId` and
nonzero `processId`. JSON status includes `audioSource` for the applied selection.
The silent Opus track remains negotiated so resuming needs no peer reconnect; this
does not disable viewers' playback devices or promise zero audio network traffic.

`ScreenShare --room-v2 CONFIG.json` uses the shared v2 RoomSession and production
Windows capture/audio runtime. Existing CLI commands and the UI remain on their
current path until cutover acceptance. Requires the pinned native build and a
Windows graphical session for capture/preview. No deployment is performed.
The same configuration can now launch the opt-in Qt session window with
`ScreenShareUi --room-v2 CONFIG.json`; see [ROOM-UI.md](ROOM-UI.md). Configuration
validation lives in `frontend/shared/RoomSessionConfig`, shared by both frontends.
Viewer `roomId` accepts either a raw identifier or `screenshare://room/v2/ROOM_ID`.
The link never overrides `origin` and carries no password or membership token.
Keep the service origin configured explicitly and supply any password separately.
Unknown versions, embedded credentials, URL queries/fragments and encoded IDs fail
local validation. The real-Worker CLI media test joins using this link form.

Optional `stream.aggregateUploadBps` sets the host's shared upload allowance
(160,000–1,000,000,000 bits/s), including in timed full-settings changes. Omit it
to disable. The backend reserves 20% plus 128 kbps audio per viewer, shares remaining
video equally, and respects the individual cap. Too little allowance pauses video;
audio continues. Status reports allocated/applied caps and nullable measured WebRTC
transport rates separately. See [ROOM-UI.md](ROOM-UI.md#shared-upload-allowance) for
the allocation rules and measurement limits. This is not an interface shaper.

Create a host configuration using your v2 service's HTTPS origin:

```json
{
  "origin": "https://your-service.example",
  "host": true,
  "nickname": "Host",
  "name": "Gaming session",
  "public": false,
  "viewerLimit": 4,
  "capture": { "display": 0, "fps": 60 },
  "audio": { "source": "system" },
  "stream": {
    "preset": "gaming",
    "resolution": "fixed",
    "width": 1920,
    "height": 1080,
    "fpsMode": "manual",
    "fps": 60,
    "bitrateMode": "manual",
    "bitrateBps": 12000000
  }
}
```

Run `ScreenShare --room-v2 host.json`. Read the assigned `roomId` from its JSON
status output, then use it in the viewer configuration:

```json
{
  "origin": "https://your-service.example",
  "host": false,
  "roomId": "ROOM_ID_FROM_HOST",
  "nickname": "Viewer",
  "preview": true,
  "seconds": 60
}
```

Run `ScreenShare --room-v2 viewer.json`. Omit `seconds` or set it to zero to run
until Ctrl+C/Ctrl+Break or viewer-window close. `seconds` includes admission time.
`preview: false` disables the viewer window; it does **not** mute normal playback.
The CLI's real sessions use normal selected audio devices. Only the test harness
substitutes silent synthetic endpoints. Unlisted rooms still require knowledge of
the room ID; add the same `password` to both files when password gating is wanted.
Protect configuration files containing passwords. Output does not echo them or
membership credentials.

Capture accepts `display` (enumerated index) or `window` (a decimal/hex HWND string,
e.g. `"0x123456"`), never both. Use existing `--list`/UI enumeration to identify
sources. Native capture dimensions are retained; per-viewer adaptation controls
output dimensions. Capture FPS defaults to initial stream FPS; set `capture.fps`
high enough for scheduled changes. Live settings do not reconfigure the device.

Audio fields are `source: system|microphone|process|none`, optional `deviceId`, optional
`playbackDeviceId`, and a required positive `processId` for process output. Device
IDs are the Windows endpoint IDs from the existing `--list-audio-devices` command.

Stream fields support `preset: gaming|quality`, `resolution: auto|fixed|native`,
`fpsMode: auto|manual`, and `bitrateMode: auto|manual`. Manual bitrate requires
`bitrateBps`; it limits the sender without forcing padding or a minimum bitrate.
Fixed resolution selects the requested canvas; content is fitted into it. Actual
delivered FPS/bitrate can fall below requested values under load/congestion.

For repeatable runs without keyboard/mouse input, add up to 64 ordered changes:

```json
"changes": [
  {
    "atMs": 5000,
    "stream": {
      "resolution": "fixed", "width": 1280, "height": 720,
      "fps": 30, "bitrateMode": "manual", "bitrateBps": 4000000
    }
  }
]
```

Each `stream` is a complete preference object using defaults for omitted fields,
not a patch. Times are milliseconds since starting the CLI session. A due change
waits for Active status and is retried if the command queue is temporarily busy.
Unknown fields, wrong types, invalid modes, unordered changes and oversized files
(over 64 KiB) are rejected. Production CLI accepts HTTPS origins only; there is no
command-line plaintext bypass.

Standard output is newline-delimited JSON. `status` contains room phase/error,
peer counts, requested settings revision and per-peer sender-applied/source-frame
observed revisions, rejection and dimensions. `settings` reports command acceptance,
not remote presentation. `admission-ended` preserves cancellation uncertainty.
Nonzero exit means admission/runtime/settings command failure. Peer-level settings
rejection is reported in status and preserves that peer's previous working settings.
Status checks read local snapshots; they generate no service polling requests.

The preview retains only the latest decoded frame and converts I420 to NV12 on the
window thread. This bounds backlog and avoids rendering on decoder callbacks.
This first frontend path uses CPU conversion; zero-copy presentation, default UI adoption,
remote input, live audio-device switching, ICE-server configuration and remote
network/latency acceptance remain outstanding. The opt-in browser provides the
directory/profile workflow, and shared upload allocation is integrated.

## Timed capture-source changes

Host configurations may include up to 64 ordered `captureChanges`, independently
of stream-settings changes. Each entry requires `atMs` and exactly one `display`
(index 0–63) or `window` (decimal/hex handle string), with optional `fps` (1–240,
default 60). For example:

```json
"captureChanges": [
  { "atMs": 5000, "display": 1, "fps": 60 },
  { "atMs": 10000, "window": "0x123456", "fps": 30 }
]
```

Times are relative to session start; operations wait for active admission and the
prior source operation to finish. The same configuration works in the opt-in Qt
window. Success reports a new `captureRevision` after the replacement's first
frame. The CLI emits `capture` results and aborts a scripted run on failure, while
interactive UI errors leave the previous healthy source running. Stop resolves
pending work and reports `capture-ended` when applicable. Audio selection does
not change. No membership refresh, room recreation or new admission is needed.

## Timed shared-audio changes

Host configurations can supply up to 64 strictly ordered `audioChanges`, using
the same public operation as the UI:

```json
"audioChanges": [
  { "atMs": 5000, "source": "microphone", "deviceId": "" },
  { "atMs": 10000, "source": "process", "processId": 1234 },
  { "atMs": 15000, "source": "system" }
]
```

An empty/omitted device ID selects the default endpoint. Process capture requires
a nonzero process ID and no device ID; other sources reject a process ID. Times
are relative to session start. Recording must already be active; schedule changes
after viewers connect. There is one pending audio operation, with no automatic retry.
The CLI emits `audio` results and `audioRevision` in status; a failed scripted change
ends the run. Pending shutdown completion is reported as `audio-ended`. UI failures
leave the session running. No credentials or endpoint identifiers are logged.

The shared parser also applies these selection checks to startup audio. These
changes affect host capture only.

## Viewer playback changes

Status now includes `audioHealth` and `playbackHealth`, each containing `state`
(`inactive`, `running`, `silent`, `failed`) and cumulative `failures`. An endpoint
startup/live failure alone preserves video and the room; a later timed command can
retry the same source/device. There is no automatic retry and command failures still
follow the existing exit policy. See [AUDIO-RECOVERY.md](AUDIO-RECOVERY.md).

Startup audio accepts `playbackDeviceId`, `playbackVolume` (0–100, default 100) and
`playbackMuted` (default false). Viewers may also supply at most 64 strictly ordered
`playbackChanges`:

```json
"playbackChanges": [
  { "atMs": 5000, "muted": true },
  { "atMs": 10000, "deviceId": "", "volume": 50, "muted": false }
]
```

Each entry is a complete selection: omitted device ID means default output,
omitted volume means 100%, and omitted muted means false. The command requires
active playback, uses no automatic retry, and emits `playback` results plus status
`playbackRevision`, `playbackVolume` and `playbackMuted`. Device IDs are not logged.
A failed scripted command ends the run; shutdown reports `playback-ended` for an
outstanding command. The interactive UI preserves the session and previous healthy
output on failure. Device initialization may pause local sound; mute does not stop
video, audio reception or the room connection. Host playback changes are rejected.

## Presentation diagnostics

The v2 preview retains packed decoder NV12 directly, uses one latest pending frame,
and enables nonblocking presentation with a measured DXGI queue limit of one.
Padded NV12 is explicitly packed; other formats use a conversion fallback. Busy or
occluded frames are dropped rather than retried. The legacy vector-based preview
entry remains compatible, while the v2 path passes immutable retained pixel storage.

At exit the shipped CLI emits a `presentation` JSON record with `received`,
`replaced`, `retained`, `converted`, `repacked`, `presented`, `dropped` and
`maximumFrameLatency`. With preview disabled there is no conversion/presentation;
only the latest received frame is retained until shutdown. Counts describe local
handoff/presentation operations, not physical display latency. Software decode and
CPU-to-GPU upload remain; the record does not imply GPU zero-copy decoding.

## Shared renderer and preview recovery

ReceiverPreviewWindow is now a Win32 window/control adapter around the same
backend-owned renderer/session as the UI. Its duplicate D3D device, shader,
texture, viewport and swap-chain pipeline has been removed. It retains fit/1:1,
F11/Alt-Enter fullscreen, Escape, mute/volume callbacks, first-frame sizing and both
legacy/retained NV12 entry points. Invalid shapes are rejected before window sizing.
Redraw is separate from fresh-frame accounting; minimized frames are discarded.

Device-loss recovery permits three rebuilds, with 250 ms drop-only backoff. Good
frames do not replenish the budget; a fourth or nonrecoverable failure becomes
terminal. The title then displays a rejoin instruction, and the shipped v2 CLI emits
a bounded `presentation-status` record per error transition. Audio and membership
continue. The final `presentation` record now includes `errors`, `recoveries` and
`terminal`. Ordinary status updates cannot overwrite the terminal title.

Explicit ClearFrame resets resources and the session recovery budget. Native
resources are released before window destruction. Closing a preview no longer
posts thread-wide WM_QUIT, so other previews/Qt windows can continue pumping.
Win32 callbacks contain exceptions rather than unwinding through system code.
Titles update only when changed, avoiding a SetWindowText call for every frame.

Windows coverage injects failures after real GPU work and during resize, checks
recovery/exhaustion/clear, scaling/fullscreen/minimize/restore, control callbacks,
malformed frames, legacy-frame compatibility and closing one of two previews.
Only direct messages to test-owned HWNDs are used; no physical input or sound.
Physical driver removal/hangs and external image/input latency remain separate gates.

## Automated checks

`room-v2-cli-entry` executes the shipped CLI and verifies argument handling,
plaintext rejection and secret-free errors. `room-v2-cli-media` uses the same
parser/controller against the local Worker with synthetic capture/audio. It checks
live resolution change, decoded pixels, NV12 conversion, bounded latest-frame
retention, source/sender revisions, graceful stop and credential-free reports.
It also checks immediate cancellation and failed admission. `room-v2-cli-deployment`
starts with only the executable in a fresh directory, deploys its runtime, checks
Core/Network/WebSockets and the Windows TLS plugin, then runs entry validation.
CLI runtime deployment is independent of test targets and UI deployment.

The explicit Windows variant additionally captures a generated test window through
WGC and presents decoded frames through the actual D3D preview, with synthetic
audio and no physical input:

```powershell
node signaling-worker/tests/run-native-service.mjs build/sdk-app-release/RoomCliWindowsTests.exe build/webrtc/room-cli-windows windows-media
```

This requires an interactive Windows session. It is not registered in routine
CTest, and may need execution outside the capture-restricted sandbox. These are
correctness tests; they do not establish remote latency, NAT/TLS or resource gates.
# Sender diagnostics additions (2026-09-17)

Host peer `sender` contains videoPayloadBps, encodedFps, rttMs, jitterMs,
lossFraction, availableOutgoingBps and limitingReason. Receiver reports now include
ageSeconds from local receipt time. Unknown/stale numeric values remain null;
RTT is not end-to-end latency. Full semantics: NETWORK-DIAGNOSTICS.md.

Each host peer now includes `receiver`: sampleState (fresh/stale/unknown), width,
height, framesDecoded and decodeFps. Missing/stale values are null; FPS zero is
retained. Reports arrive over the encrypted peer channel and describe decoding,
not remote display or latency. See [RECEIVER-TELEMETRY.md](RECEIVER-TELEMETRY.md).

Viewer `presentation-status` now emits once per second plus immediate error
transitions. It includes presented/dropped counts and a `diagnostics` object,
also included in final `presentation`: `outcome`, `lastErrorCode`, `busyDrops`,
`occludedDrops`, `minimizedDrops`, `unavailableDrops` and `backoffDrops`.
Error codes are hexadecimal HRESULTs (null before errors/after explicit clear;
untyped exceptions use E_FAIL). Counters are cumulative; explicit clear resets
the recovery budget, last error and current outcome. They do not sum to total
drops: queue replacement, unknown backends and terminal/error drops also exist.
Outcome describes the last frame attempt, not continuing display freshness.
No telemetry is sent to the room service.

Status JSON now includes `requestedPreferences`; each peer includes
`requestedRevision`, `state` and `transportSampleState`. States are `pending`,
`rejected`, `upload-paused`, `waiting-for-source`, or `source-observed`.
Sample state is `fresh`, `stale`, or `unknown`; `transportSendBps` is null when
unknown/stale and numeric zero only for a fresh measured zero. Existing fields
remain compatible. These values describe host sender/source observations, not
receiver display, physical-interface throughput or a congestion diagnosis.
