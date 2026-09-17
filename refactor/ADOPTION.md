# Normal room workflows — guarded adoption

The existing home screen can now route Start Sharing, Join Room and Quick Join
through the shared v2 backend. CLI create/join no longer requires a JSON file.
This is an explicit backend choice; ordinary launch and existing UDP/control
commands remain available until feature parity and cutover acceptance pass.

## Launch

Use the origin of a service that supports the v2 endpoints; these commands do
not deploy or upgrade a Worker:

```powershell
ScreenShareUi --backend v2 --signal-server https://your-worker.example
ScreenShare --backend v2 --signal-server https://your-worker.example --create-room --name "Game night" --nickname Player --resolution 1920x1080 --fps 60 --bitrate 8000000
ScreenShare --backend v2 --signal-server https://your-worker.example --join-room ROOM_ID --password-file room-password.txt
```

The UI uses the current HomeWindow, AppShell and existing v2 form/session pages,
not another top-level application window or the later visual redesign. The
ordinary update-check scheduling and app version remain in this launch path.
The home states that controllers require host permission and mouse/keyboard remain
unavailable. See [CONTROLLERS.md](CONTROLLERS.md) for UI/CLI controller integration.

CLI create returns a server-issued room ID in its JSON status. Join accepts that
ID or `screenshare://room/v2/ROOM_ID`. The configured HTTPS origin remains
authoritative: links cannot change it or carry credentials. Ctrl+C/window close
uses the existing asynchronous shutdown; `--seconds N` bounds a CLI session.

## CLI options

| Scope | Options |
| --- | --- |
| Required | `--backend v2 --signal-server HTTPS_ORIGIN`, plus exactly one `--create-room` or `--join-room ID_OR_LINK` |
| Both roles | `--nickname NAME`, `--password-file PATH`, `--seconds 0..86400` |
| Host room | `--name NAME`, `--private`, `--viewer-limit 1..63` |
| Host capture | `--display INDEX` or `--window HWND` (not both) |
| Host video | `--preset gaming\|quality`, `--resolution auto\|native\|WIDTHxHEIGHT`, `--fps auto\|N`, `--bitrate auto\|BPS`, `--upload-bps BPS` |
| Host audio | `--audio system\|microphone\|process\|none`, `--audio-device ID`, `--process-id PID` as appropriate to the source |
| Viewer | `--no-preview`, `--playback-device ID`, `--volume 0..100`, `--mute` or `--unmute` |

Nickname, stream and playback defaults come from the existing versioned RoomV2Profile.
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

Legacy fixed ports, caller-selected room IDs, custom UDP/NAT invitations and
remote-control flags are not silently reinterpreted as v2. They still work on
their existing path when v2 is not selected. Use server-issued IDs and the new
create/join commands for v2. No compatibility claim is made for old room links.

## Home, directory and lifetime

- HomeWindow accepts pushed room rows and routing callbacks. In v2 it creates no
  legacy room-list HTTP client and performs no password preflight. Incoming room
  names are plain text, not interpreted markup.
- Home and the room form share one pushed directory subscription. Repeated
  home/form/back navigation and refresh while connected do not reconnect it.
  Sessions stop the directory; leaving resumes it. Disconnected lists cannot
  expose stale Quick Join actions. Closed/full rows are not joinable.
- Quick Join selects the room by ID and opens the join form so its password can
  be supplied. Admission still authenticates and checks capacity on the server.
- Back clears the transient password. Capture discovery refreshes when opening
  Create or on explicit refresh; missing/failed discovery cannot silently select
  display 0. The previous selection is retained only if still enumerated.
- The shell remains alive through asynchronous directory/media drain. Session
  pages are destroyed on return, navigation stays in one window, and only an
  active session keeps the display awake. Input revoke shortcuts are not claimed
  without an actual handler.

## Tests and remaining parity

The real local-Worker UI scenario now includes normal-home create, quick join,
password admission, decoded frames, return and shutdown. It asserts no legacy
HTTP client, one connection across form navigation, plain-text room names and
bounded page ownership. It uses the same production routing as the normal UI.
The CLI media scenario now creates/joins through the command parser before
running the existing real H.264/Opus/settings/source/audio scenarios. Additional
tests cover saved defaults, explicit overrides, malformed/oversized passwords,
origin/link validation and rejection of unsupported options. Actual executables
also reject missing service/unsupported legacy CLI options/plaintext UI origin.
Media endpoints in these tests are synthetic or generated-window adapters;
there is no physical input or audible test tone.

Release and Debug desktop-inclusive matrices pass **7/7 each** at
`build/webrtc/adoption-verified-{release,debug}/result.json`; runtime/UI smoke
checks pass **4/4 each**. See HEADLESS-TESTING.md for evidence and the corrected
test that originally selected the first room row instead of the intended room ID.

Default cutover remains blocked by remote keyboard/mouse/controller parity
(Stage 3), physical media/latency/resource/network acceptance (Stages 2/4), and
the final compatibility/reporting/updater audit and obsolete-path removal
(Stage 5). Legacy diagnostic ZIP reports and direct/invite CLI workflows must be
explicitly replaced or retired at that audit, not silently dropped by this flag.
The guarded share/watch workflow is implemented; full default adoption and
Stage 2 acceptance are not being marked complete.
