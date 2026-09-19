# Command-line rooms

The modular room backend is the normal CLI. Both endpoints need v2 clients and a
new room; legacy UDP commands and v1 links are not translated. See [usage](usage.md).

## CLI options

| Scope | Options |
| --- | --- |
| Required | Exactly one `--create-room` or `--join-room ID_OR_LINK` |
| Service override | `--signal-server HTTPS_ORIGIN`; optional compatibility switch `--backend v2` |
| Both roles | `--nickname NAME`, `--password-file PATH`, `--seconds 0..86400` |
| Host room | `--name NAME`, `--private`, `--viewer-limit 1..63` |
| Host capture | `--display INDEX` or `--window HWND` (not both) |
| Host video | `--preset gaming\|quality`, `--resolution auto\|native\|WIDTHxHEIGHT`, `--fps auto\|N`, `--bitrate auto\|BPS`, `--upload-bps BPS` |
| Host audio | `--audio system\|microphone\|process\|none`, `--audio-device ID`, `--process-id PID` as appropriate to the source |
| Viewer | `--no-preview`, `--playback-device ID`, `--volume 0..100`, `--mute` or `--unmute`, `--decoder auto\|software` |

Nickname, stream, playback and viewer-decoder defaults come from the existing versioned RoomV2Profile.
CLI overrides apply only to that invocation; they do not rewrite saved defaults.
Manual numeric video choices stay Manual; `auto` explicitly selects Auto.
`--bitrate auto` removes an inherited manual bitrate value. Existing backend
validation still enforces dimensions, numeric bounds and audio-role constraints.
`--no-preview` does not mute playback. Host sharing normally opens no preview.

Passwords are read from a bounded file containing one nonempty UTF-8 line
(optional terminal LF/CRLF, meaningful spaces preserved, 128 UTF-8 bytes maximum,
no control characters). Read errors never echo the filename or content. No
password command-line value, room credential persistence or diagnostic loopback
flag is exposed. Unknown, duplicate, conflicting and wrong-role options fail
before admission. JSON entry points keep their existing independent defaults.


## CLI workflow

The v2 create/join command parser accepts `--control-file PATH`; viewers also require
`--gamepad DEVICE_ID` and an enabled preview. These are command-line options, not
persisted profile or JSON configuration fields. Use a private local file and replace
it atomically with a new command. Polling is local at 100 ms, with no server requests.
Existing content at startup is ignored. Commands have strictly increasing integer
`sequence` values (1 through 2^53-1), exact allowed keys and a maximum size of 4096 bytes.

```json
{"sequence":2,"operation":"request","peer":"HOST_PEER_ID","consent":true}
```

The host writes `operation:"grant"` with the requesting viewer's exact peer ID and
`consent:true`. Either endpoint writes `operation:"revoke"`; omit `peer` to revoke all.
Status JSON exposes input readiness, requests, grants, pending state and reason.
An `input-command` result's `accepted` means local queue acceptance, not remote grant.
Invalid/unavailable commands are consumed, never automatically retried. A new
sequence and fresh explicit consent are required after failure/revoke. Preview focus
loss cancels local arming as well as remote permission. The CLI registers the same
panic shortcut; failure to register aborts a control-enabled launch.


## JSON session configuration

Use `ScreenShare --room-v2 CONFIG.json` or `ScreenShareUi --room-v2 CONFIG.json`
for diagnostic configurations. Validation is shared in `frontend/shared/RoomSessionConfig.cpp`.

Optional `stream.aggregateUploadBps` sets the host's shared upload allowance
(160,000–1,000,000,000 bits/s), including in timed full-settings changes. Omit it
to disable. The backend reserves 20% plus 128 kbps audio per viewer, shares remaining
video equally, and respects the individual cap. Too little allowance pauses video;
audio continues. Status reports allocated/applied caps and nullable measured WebRTC
transport rates separately. This is not an interface shaper.

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

The preview uses a latest-frame handoff and presents retained GPU frames when
available, with CPU conversion/upload fallback. Audio-device switching, normal
Home/CLI routing and remote input are integrated. Hardware compatibility and
external network/latency qualification remain in [known limitations](known-limitations.md).

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
follow the existing exit policy. See [runtime ownership](architecture.md).

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
CPU-to-GPU upload remain available as fallback; these counts alone do not establish
which path is active.

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
