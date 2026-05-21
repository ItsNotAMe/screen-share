# Project Memory

## User Preferences

- Build a native Windows C++ screen-sharing app for friends.
- No web app, no C#.
- Use local `git` and `gh` for GitHub work; do not use the GitHub connector for this repo.
- Keep PRs small. Self-review before opening or merging.
- Avoid branch, commit, PR, or tracked-file text that depends on one contributor's local tooling.
- After each PR merge, mention the next recommended step.
- Durable repo memory lives under `agents/`; update it when project direction or implementation facts change.

## Current State

- `main` is synced to `origin/main` at PR #94, `d81b13b Merge pull request #94 from ItsNotAMe/feature/signaling-worker`.
- Active branch: `main`.
- The app builds with CMake debug/release presets and produces `ScreenShare.exe`.
- Normal/default CMake builds now also create portable zip packages.
- The app can also build optional `ScreenShareUi.exe` when Qt 6 Widgets is available.
- `scripts/install-dev-deps.ps1` bootstraps Windows dev dependencies: MSYS2 native packages, optional Qt/FFmpeg, Node.js LTS, and signaling Worker npm packages.
- Current stable live run shape:

```powershell
.\build\release\ScreenShare.exe --udp-recv 5000 --preview --audio-playback --log receiver.log
.\build\release\ScreenShare.exe --display 0 --seconds 18000 --udp-send HOST:5000 --adapt-bitrate --adapt-resolution --audio-capture system --log sender.log
```

## Current Pipeline

```text
WGC capture by default
 -> D3D11 scaling / HDR handling / GPU NV12
 -> H.264 stream encoder; `--share` defaults to software for stability, raw stream encoding can still use auto/hardware
 -> paced UDP sender with feedback and optional adaptation
 -> receiver UDP reassembly and keyframe-aware H.264 decode
 -> D3D11 preview window with paced playout
 -> WASAPI system/mic capture, Opus by default
 -> receiver audio playback with A/V sync enabled by default for preview+audio
```

## Recent Merges

- PR #42 `Stage MinGW runtime DLLs`: static MinGW runtime plus staged Opus/UCRT/D3DCompiler DLLs.
- PR #43 `Trim delayed audio playout backlog`: audio playout backlog trimming.
- PR #44 `Use stable live streaming defaults`: Opus default and A/V sync default for preview+audio.
- PR #45 `Stabilize live A/V playout`: log files, sync fixes, sender queue diagnostics, adaptation pressure tuning.
- PR #46 `Add portable zip package target`: automatic portable zip in normal builds and explicit `package-portable`.
- PR #47 `Add live session presets`: `--watch PORT` and `--share HOST:PORT` shortcuts for common live sessions.
- PR #48 `Add save report command`: global `--save-report PATH` to capture one run's console output into a zip report with runtime info.
- PR #49 `Add shared diagnostic session metadata`: `--session` IDs, report fingerprints, and receiver session fingerprints in feedback.
- PR #50 `Add receiver feedback summary to reports`: sender reports include the latest observed receiver health snapshot when feedback is available.
- PR #51 `Add receiver preview window controls`: fullscreen, fit/1:1 scaling, and source-size resize shortcuts.
- PR #52 `Add receiver audio playback controls`: muted loopback-safe receiver playback, volume controls, audio status telemetry, and DXGI Alt+Enter handling fix.
- PR #53 `Add Qt control UI`: optional Qt desktop UI for Share/Watch presets, Start/Stop, live output, report creation, packaging updates, graceful stop-file shutdown, and video-only fallback when automatic sync sees video but no audio.
- PR #54 `Add LAN receiver discovery`: opt-in `--lan-advertise`, `--lan-discover`, UI Watch LAN discoverable checkbox, UI Share Find on LAN button, and directed IPv4 broadcasts for real LAN discovery.
- PR #55 `Add local access code gate`: `--access-code` / `--session-code` for matching local sessions, packet fingerprint filtering, UI access-code field, telemetry, and report/command redaction.
- PR #56 `Encrypt local UDP sessions`: access-code-derived AES-GCM encryption for video/audio/feedback payloads, crypto rejection telemetry, and report/README updates.
- PR #57 `Improve encrypted session UX`: random access-code generation, explicit plaintext acknowledgement, plaintext warnings, and UI Generate/Copy/security-choice flow.
- PR #58 `Advertise LAN session security metadata`: discovery reports encrypted/plaintext receiver state, access-code fingerprint metadata, safer generated commands, and UI Find-on-LAN session/security hints.
- PR #59 `Simplify LAN discovery access-code flow`: removed the separate invite-code direction, moved access-code fingerprinting into shared UDP crypto, and made the UI compare the typed access code to the receiver's advertised fingerprint.
- PR #60 `Stabilize share sender queue adaptation`: `--share` live UDP queue cap, sender-queue resolution pressure, 125% video pacing headroom, and software encoder default for `--share` after hardware MFT input drops were confirmed.
- PR #61 `Quiet idle receiver telemetry`: receiver logs one `waiting_for_stream` line while idle instead of repeating full zero-value stats.
- PR #62 `Keep receiver sync catch-up real-time`: preview is the live timeline, audio catch-up can drop queued audio, and video safety gating avoids reciprocal sync deadlocks.
- PR #63 `Keep preview running without audio`: automatic video-only fallback stops using stale audio renderer state as a preview/audio gate.
- PR #64 `Add receiver discovery list to UI`: Share tab has an auto-refreshing Receivers list, relaxed 15-second background refresh, de-duped loopback/LAN entries, automatic UI sessions, single-field access-code retry behavior, tighter settings layout, and no wheel focus/input changes while scrolling.
- PR #65 `Warn before sharing to localhost`: UI warns before starting Share to loopback targets like `127.x.x.x`, `localhost`, or `::1`, while still allowing explicit local tests.
- PR #66 `Add STUN endpoint diagnostic`: standalone `--stun HOST[:PORT]` command sends a STUN Binding Request and prints local/public/server/manual invite UDP endpoints.
- PR #67 `Add audio output device selection`: Share UI output-device picker, `--audio-device-id` command wiring, and multichannel capture downmix before Opus.
- PR #68 `Add Tailscale peer picker to UI`: Share UI Targets list includes optional online peers from `tailscale status --json`; these are quick targets, not confirmed ScreenShare receivers.
- PR #69 `Add NAT invite generation command`: `--make-invite` emits manual invite blobs with public/local endpoint and security metadata.
- PR #70 `Add NAT probe exchange diagnostic`: `--nat-probe` sends probe/reply packets to peer invite endpoints for manual UDP hole-punch testing.
- PR #71 `Allow UDP sender local port binding`: `--udp-local-port` binds Share/live UDP send to a known local port.
- PR #72 `Send NAT punch probes from Watch socket`: Watch/UDP receive can use `--peer-invite` to send NAT punch probes from the real receive socket while waiting for media; sender feedback ignores probe datagrams.
- PR #73 `Allow Share to target NAT invites`: `--share "nat_invite=..."` resolves the receiver invite public endpoint, validates invite security, and documents the two-invite manual flow.
- PR #74 `Add NAT invite endpoint selection`: `--invite-endpoint auto|public|local` lets Share choose the invite endpoint for manual NAT/LAN/VPN experiments; local endpoint startup was validated with an approved one-second Share run.
- PR #75 `Retarget Share from NAT probes`: Share auto mode can retarget its UDP media destination to the source endpoint of incoming Watch NAT probe datagrams; access-code fingerprints reject mismatched retarget attempts.
- PR #76 `Bind Share sender from local NAT invite`: Share `--local-invite INVITE` binds the sender UDP socket to the local port inside this side's invite, reducing the chance that live NAT tests forget the matching `--udp-local-port`.
- PR #77 `Add NAT setup status diagnostics`: sender/receiver logs include `nat_status` / `nat_hint` fields.
- PR #78 `Print guided NAT invite commands`: `--make-invite` prints Watch/Share/probe command templates using `<PEER_INVITE>` and `CODE` placeholders.
- PR #79 `Add NAT invite fields to UI`: Share can paste receiver/local invites, Watch can paste sender invites, and the command preview masks invite blobs.
- PR #80 `Add UI invite creation flow`: Share/Watch can create and copy their own local NAT invite blobs from the app.
- PR #81 `Guide NAT invite exchange in UI`: paste/extract buttons, compact status hints, and Share-side guardrails for receiver invite flow.
- PR #82 `Reset receiver pipeline on stream restart`: Watch now recovers when Share stops and starts again without restarting Watch.
- PR #83 `Show live NAT status in UI`: Internet hints switch to live probe/media states from engine `nat_status` / `nat_hint` output while NAT invite mode is running.
- PR #84 `Add UI invite test checklist`: README has a two-computer checklist for future UI invite/Tailscale validation.
- PR #85 `Add direct NAT invite sharing flow`: merged the full direct STUN/manual invite/UDP hole-punching flow into `main`.
- PR #86 `Clarify room UI setup`: Share/Watch wording, segmented connection tabs, display/audio chooser polish, header status pill, receiver stale-preview blanking, and Windows UDP reconnect resilience for late/restarted Watch.
- PR #93 `Add NAT multi-viewer room invite targets`: one sharer room invite, optional watcher response invite list, NAT probe-learned fanout through one sender socket, and direct multi-target Share UI cleanup.
- PR #94 `Add Cloudflare signaling worker scaffold`: signaling-only Worker project, Windows dependency bootstrap script, Worker lockfile/typecheck config, and hidden room-key direction docs.
- Current room-invite UI work keeps the project no-server/no-external-service: Create room owns one invite and Join room pastes it, but real NAT testing showed this only works for LAN/VPN/reachable-NAT. Endpoint-filtered NAT can block the watcher probes before Share sees them.

## Active Memory Files

- `agents/todo.md`: single source for upcoming tasks and backlog.
- `agents/repo-map.md`: source layout and build shape.
- `agents/live-pipeline.md`: sender/receiver pipeline and important stats.
- `agents/av-sync.md`: A/V sync behavior and test command shape.
- `agents/adaptation.md`: adaptive bitrate/resolution notes.
- `agents/packaging.md`: runtime DLL and portable zip notes.
- `agents/ui.md`: Qt desktop control UI notes.
- `agents/lan-discovery.md`: LAN discovery protocol and test notes.
- `agents/nat-traversal.md`: STUN/manual invite/hole-punching direction.
- `agents/security.md`: local access-code and future encryption notes.
- `agents/signaling.md`: signaling backend direction and room-flow constraints.

## Current Direction

- The remaining freeze/stale-frame work is intentionally backlog, not the default next PR.
- PR #47 merged the first run/session preset slice:
  - `--watch PORT` expands to receiver preview plus audio playback.
  - `--share HOST:PORT` expands to UDP video send, system audio capture, adaptive bitrate, and adaptive resolution.
  - `--share` defaults to infinite run time; `--seconds S` still overrides it for tests.
- Report bundle support is merged:
  - `--save-report PATH` captures the current run's console output into a zip with `ScreenShare-report.txt`, `logs/console.log`, and the runtime dependency manifest when available.
  - `--log PATH` remains useful for plain text logs and can be combined with `--save-report`.
- Shared session metadata is merged:
  - Add global `--session ID` / `--session-id ID` with an automatic generated session when omitted.
  - Save session ID and fingerprint in `ScreenShare-report.txt` and console telemetry.
  - Include the receiver session fingerprint in existing feedback packets, appended for compatibility with the previous feedback packet shape.
  - Sender stats report `udp_feedback_session`, which can be matched to the receiver report's session fingerprint.
  - Elevated localhost live loopback validated that the sender sees the receiver fingerprint in feedback.
- Receiver feedback report summary is merged:
  - Preserve the latest sender-observed receiver feedback snapshot in the saved-report context.
  - Add a compact `Latest receiver feedback` section to `ScreenShare-report.txt`.
  - Elevated localhost sender/receiver validation confirmed sender reports include receiver health, completed frames, and receiver session fingerprint.
- Receiver UX controls are merged:
  - Preview: F11/Alt+Enter fullscreen, Esc exit fullscreen, F fit/1:1, 1 source-size resize.
  - Audio: `--audio-playback-muted`, `--audio-playback-volume PERCENT`, M mute, + and - volume.
  - DXGI's default Alt+Enter handling is disabled so the app-owned fullscreen restore keeps the title bar.
- Qt control UI is merged:
  - optional `ScreenShareUi.exe` when Qt 6 Widgets is available.
  - dark-mode default with theme toggle.
  - Share/Watch presets, Start/Stop, command preview, live output, session/report controls.
  - portable zip includes Qt plugin folders and transitive runtime dependencies.
  - Stop now uses a hidden stop-file signal so the engine exits cleanly before force-kill fallback.
- LAN discovery is merged:
  - `--lan-advertise` on watch/receive mode.
  - `--lan-discover` search mode.
  - UI Watch LAN discoverable checkbox and Share Find on LAN button.
- LAN discovery security simplification is merged:
  - One access code remains the user-facing secret.
  - Discovery advertises encrypted/plaintext state and fingerprint metadata, never the raw access code.
  - The UI can compare the typed access code against the discovered receiver fingerprint.
- Receiver discovery UI is merged:
  - Share has an auto-refreshing Receivers list plus manual Refresh.
  - Selecting a discovered receiver fills address/port; encrypted receivers use the main Access code field only.
  - Wrong discovered access codes clear/focus the Access code field for direct retry.
  - The UI warns before Share starts with localhost/loopback targets.
- Tailscale peer picker is merged:
  - It uses optional local `tailscale status --json` output in the UI.
  - Entries are quick targets only; they do not prove Watch is running and do not carry ScreenShare security metadata.
  - Keep this separate from UDP LAN discovery.
- Direct NAT traversal building blocks are merged to `main`:
  - `--stun HOST[:PORT]` defaults to port 3478.
  - `--stun-timeout-ms MS` controls the query timeout.
  - Validated against `stun.l.google.com:19302`; output includes local and public UDP endpoints.
  - `--make-invite`, `--nat-probe`, `--udp-local-port`, Watch `--peer-invite`, Share `--share "nat_invite=..."`, `--invite-endpoint auto|public|local`, Share `--local-invite`, and Share auto retarget from Watch probes are now available for manual hole-punch experiments.
  - `--make-invite` prints guided command templates for Watch, Share, and optional probe diagnostics.
  - Current generated invites are compact by default: `ss1e:` is encrypted with the shared access code and hides endpoint/session metadata; `ss1p:` is compact plaintext for explicit plaintext mode. Legacy verbose `nat_invite=screenshare-invite-v1;...` strings still parse.
  - Current UI bridge presents one sharer-owned room invite for LAN/VPN/reachable-NAT and an optional Watch-side My invite response for blocked NAT pairs. Share uses the friend response as `--share` and its own room invite as `--local-invite`.
  - Guided UI work adds create/copy/paste/extract buttons and compact status hints for the one-invite room flow.
  - NAT logs now summarize setup state with `nat_status` / `nat_hint` so reports can distinguish missing probes, rejected probes/media, retarget-without-feedback, and receiving states.
  - Remaining NAT work is deferred unless reports show direct hole punching is insufficient or diagnostics are still confusing.
- Receiver restart work:
  - This is not NAT-specific; the receiver should recover whenever a sender stops and starts again while Watch remains open.
  - The fix detects a video frame-id rewind with a newer sender QPC clock and resets decode, preview, audio playout, A/V sync, and receiver media queues.
  - Debug loopback validation showed `receiver_stream_restart`, one H.264 decoder restart, and continued decode across two sequential sender runs.
- UI live NAT status is merged:
  - The Qt UI can parse engine `nat_status` / `nat_hint` output while Share/Watch is running.
  - The Internet hint should switch from setup guidance to live states like probing, probe seen, connected, receiving, or rejected.
- NAT validation state:
  - User already validated the UI-guided invite flow after create/copy invite landed.
  - Later changes did not alter the NAT invite/probe/media mechanics, only receiver restart recovery and UI status display.
  - README has a two-computer UI checklist for future invite/Tailscale validation runs.
- Latest audio failure diagnosis:
  - Tailscale/transport was healthy; receiver got audio packets and playback was active.
  - The sender captured an 8-channel SteelSeries virtual output, but receiver Opus payloads were tiny silence-like packets, pointing to the wrong output/default device rather than packet loss.
  - Keep a small multichannel-to-stereo downmix safeguard, but the real UX fix is making the Share UI's output-device selection obvious.
- PR #60 fixed the freeze issue:
  - User confirmed it works and audio sounds fine too.
  - Reports showed the bad path was not LAN discovery/plaintext/encryption or native resolution alone.
  - The NVIDIA hardware H.264 MFT built an input queue and dropped frames; local tests showed software had zero `stream_dropped` at 640x360, 1280x720/60, and native 2560x1440/60.
  - `--share` now defaults to software encoding while `--stream-encoder hardware` remains available for experiments.
- Multi-viewer direction:
  - First slice is direct UDP fanout: one encoded video/audio stream is sent to multiple direct `HOST:PORT` targets using separate `UdpSender` instances.
  - Each target owns its own UDP socket, queue, and feedback path; sender telemetry aggregates counters and reports `udp_targets`, `udp_active_targets`, and `udp_failed_targets`.
  - Runtime send/feedback/flush errors on one target should mark only that target failed and keep the remaining viewers alive.
  - The Qt UI exposes direct multi-target sharing through Nearby multi-select and Manual comma/space-separated target lists.
  - NAT multi-viewer direction is one shared sharer room invite plus an optional watcher response invite list. Share binds the sharer invite's local port, learns watcher endpoints from valid NAT probes, sends outward to any pasted watcher response invite endpoints, and fans out the encoded stream through the same sender socket. Per-watcher sharer-local invite rows are intentionally not the main UI model.
  - Remaining multi-viewer work is per-viewer health and optional per-viewer bandwidth policy.
- Signaling direction:
  - First backend lives under `signaling-worker/`.
  - It is Cloudflare Worker + Workers KV for room membership, peer UDP candidates, heartbeat, and cleanup only.
  - It must not relay media or receive raw room keys/passwords.
  - Future native room UX should generate a hidden room key automatically so users get encrypted UDP media without seeing an access-code field.
  - Native C++ diagnostic integration started with `src/transport/SignalingClient.*` plus `--signal-health`, `--signal-join`, `--signal-peers`, `--signal-heartbeat`, and `--signal-leave`.
  - Live Share/Watch CLI integration is in progress: `--watch PORT --signal-server URL --signal-room ROOM` publishes the watcher candidate and turns returned peers into NAT probe targets; `--share-room PORT --signal-server URL --signal-room ROOM` publishes the sharer candidate and turns returned peers into UDP send targets.
  - Runtime live signaling now periodically rejoins the room as heartbeat/polling. Share can start before Watch and wait for peers; Watch can add newly discovered room peers as NAT probe targets; Share can add newly discovered watcher candidates to the active sender socket.
  - Remaining signaling TODO is UI wiring for the Worker room flow, hidden room-key encryption, and real multi-computer validation.
