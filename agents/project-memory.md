# Project Memory

## Current integration evidence — 2026-09-16

RoomManagedPeer now joins authenticated negotiation to HostPeerOwner's scheduled
recovery and capture-cleanup barriers in the four-peer scenario. Capture attachment
and retirement are asynchronous on signaling. BeginStop shares completion across
requests and drains capture without blocking signaling; use it before destruction.
Normal UI/CLI facade dispatch is still unfinished. See CHECKPOINT-B.

RoomNetwork now owns one Qt network loop for admission/room sockets, asynchronous
commands/cancellation, bounded queues and terminal overflow. RoomPeerRoster drives
actual media lifecycle from authenticated snapshots in RoomMediaProof, including
socket reconnect and kick/rejoin. No caller Qt event pump is needed. The diagnostic
still waits on command/capture futures; shared automatic facade dispatch, cleanup
barriers and recovery-policy integration remain open. See CHECKPOINT-B.

RoomMediaProof now runs four actual H.264 peers and synthetic Opus against isolated
workerd via native admission/sockets and shared RoomPeerNegotiation. Slow-viewer
isolation, twelve encrypted channels, fresh-ID restart, stale candidate rejection,
kick/rejoin and close pass. Full Debug/Release proof suites: 28/28; Release app:
13/13; CLI-only: 8/8. The normal UI/CLI facade remains legacy and automatic roster/
failure/recovery ownership is still open. Local evidence does not close latency,
resource, remote-network or cost gates. See CHECKPOINT-B and HEADLESS-TESTING.

## User Preferences

- Native sources are now split into root-level backend/ and frontend/ (UI, CLI,
  updater). Keep backend include paths independent of frontend. Existing include
  spellings are retained through target-specific roots; no compatibility src tree.

- Work in the end-to-end delivery milestones in refactor/TODO.md, not one helper/check per turn. Detailed checks moved to refactor/DETAIL-CHECKS.md and are acceptance details, not stopping points. Carry integration and meaningful failure scenarios together; intermediate commits do not end the batch.

- Use the patch editing tool for source/document changes so edits are reviewable in Codex; do not use PowerShell/Python file rewrites. Continue committing at logical milestones. Committed work can be reviewed using the commit or branch diff rather than only unstaged changes.

- Build a native Windows C++ screen-sharing app for friends.
- No web app, no C#.
- Use local `git` and `gh` for GitHub work; do not use the GitHub connector for this repo.
- Run local `gh` commands with escalated permission from the start; do not first try them in the sandbox.
- Keep PRs cohesive and reviewable; bigger PRs are fine when they advance one clear step. Self-review before opening or merging.
- Commit at substantial logical milestones during implementation; avoid tiny commits and accumulating completed milestones without a commit.
- Continue through larger feature milestones in one turn; intermediate commits are checkpoints, not reasons to stop and require another “continue”.
- Prefer the cleanest simple design when it has no real product or maintenance downside, even if it takes more implementation effort.
- Do not keep compatibility/fallback code for a feature that is being replaced unless there is a concrete product or diagnostic reason.
- Avoid branch, commit, PR, or tracked-file text that depends on one contributor's local tooling.
- After each completed work step or PR merge, mention the next recommended step.
- During revamped UI work, do not grow one giant UI file. Split code into logical screens/widgets/styles/adapters while keeping the implementation simple.
- When a new UI screen first runs, pause and ask the user for a screenshot so the visual design can be validated before moving on.
- Durable repo memory lives under `agents/`; update it when project direction or implementation facts change.

## Current State

- Compiled Qt RoomAdmission/RoomSocket now have a one-command workerd integration harness (signaling-worker/tests/run-native-service.mjs), also registered with CTest when dependencies exist. Snapshot events carry accepted revisions for safe edits. Coverage joins native/service admission, directory push/resync, edits/conflicts, synthetic signaling, reconnect, visibility and closure. The actual application coordinator/media facade still needs adoption; remote TLS and media latency are not established.

- Directory/capacity delivery is now a bounded background outbox outside room input/state gates. A shared state serializer and version-matching acknowledgements protect newer changes and closure; the latest pending value survives failures for alarm retry. Actual workerd tests cover stalled service with concurrent control/signaling, late ACKs, close races and the real 5s abort. Normal native/media integration and production hibernation/load/queue acceptance remain pending.

- V2 directory completes the isolated service's six endpoints: SQLite snapshot/pushed deltas, no per-room listing fanout, publish-after-host-attach, persisted versioned retries, closure tombstones, 60s renewals/180s leases and silent lease-only updates. Actual workerd tests cover failures/retry, lifecycle/visibility, expiry and 500 max-name rows. Native/media adoption, hibernation/load/queue pressure and production cutover remain open.

- V2 directed signaling now enforces pair roles, current socket generations, fresh per-offer connection IDs (including restart), candidate limits and bounded recovery/history. Workerd tests relay offer/answer/ICE/restart without room revision changes and reject stale/cross-viewer traffic. Native media adapter must map its negotiation generations to fresh wire IDs; directory, production queue pressure and cost acceptance remain pending.

- V2 room mutations now enforce socket-derived host/viewer permissions, expected revisions, last-32 request deduplication and a 120-message/minute metadata budget. Live workerd tests cover profile/policy updates, reduced capacity without eviction, kick/leave token invalidation and host closure. Saves preserve earlier alarm deadlines. Directory/signaling and normal application integration remain open.

- Isolated v2 Worker admission and socket membership now live under signaling-worker/src/v2 with wrangler.v2.toml. Real Miniflare/workerd tests cover credentials, serialized joins, provisional alarm cleanup, socket replacement/reconnect, automatic pong and the global 500-room cap. Default v1 remains active and nothing is deployed. Directory, command/signaling authorization, per-socket bounds, native/media integration and real hibernation scheduling remain open; see refactor/CHECKPOINT-C.md.

- Native RoomAdmission now implements single-flight asynchronous create/join with HTTPS origin validation, bounded responses, no redirects/cookies/cache, exact credentials/role checks and unconfirmed cancellation/failure results without application retries. Success returns a same-origin RoomSocket config with expectedRole; conflicting snapshots reject. Actual HTTP and role-mismatch socket tests are in the application/headless suite. The exact future v2 service contract is in `refactor/ROOM-PROTOCOL.md`; service handlers/expiry, media routing and normal UI/CLI adoption remain open.

- ScreenShareRoom now shares validators/cache and private Qt RoomSocket across backend builds, including CLI-only builds. Live tests on a separate QThread cover headers, snapshot/resync, target/role checks, actual heartbeat/reconnect, directory lifecycle and write pressure. Remote endpoints require wss; plaintext is an explicit numeric-loopback diagnostic option. Normal UI/CLI, admission, media connection-ID dispatch and v2 Worker integration remain open. See `refactor/CHECKPOINT-C.md`. `--room-build-directory` adds this test to the headless runner; `run-webrtc-proof.ps1 -Application -NoUi` supports separate CLI-only validation.

- HostPeerOwner now binds HostPeerRegistry to SignalingExecutor and automatically advances deadlines, ready peer operations, restart and capture cleanup every 20 ms on signaling. IMediaPeer::Poll must never wait/reenter; failures are isolated and reported as operationFailed. Four-peer diagnostics now negotiate concurrently, then complete restart/rejoin through this owner instead of manually ticking the registry or synchronously transferring restart SDP. Owner/capture must be destroyed before the executor; delayed callbacks hold weak state. Authenticated room delivery and UI/CLI facade remain open. See `refactor/CHECKPOINT-B.md`.

- SignalingExecutor now owns the native WebRTC event loop in the shared application/proof library. It admits 64 pending commands, reports typed outcomes, cancels queued work on stop and joins externally; accepted closures are destroyed on signaling. Real single/four-peer media proofs use it, and a separate offer/answer test proves progress without manual pumping. The application peer owner, room transport and UI/CLI facade remain next. Close/destroy peers on the executor before stopping; never join it from its own thread. See `refactor/CHECKPOINT-B.md`.

- PeerNegotiation now implements asynchronous SDP creation/application with typed futures, weak callback cancellation and one in-flight operation, in a shared application/proof library. The old proof description observers are removed; real media/restart tests use the adapter. Application executor and authenticated room delivery remain next; UI/CLI media has not switched. See `refactor/CHECKPOINT-B.md`.

- HostPeerRegistry now optionally binds HostMediaSession for automatic asynchronous failure/removal cleanup. Tick retries queue pressure, snapshots expose pending/error state, and bound Remove is accepted asynchronously (wait for row disappearance before reuse). Full Stop joins capture first; capture must outlive the registry. The four-peer proof covers terminal failure cleanup/rejoin; application executor and room adapter integration remain open.

- HostPeerRegistry/IMediaPeer now own real four-peer proof connections and dispatch queued restart/close actions with generation validation and retained terminal snapshots. Adapter shutdown is idempotent; capture stops before peer release. Application signaling scheduling, room delivery and capture cleanup on peer failure remain open. Admission is in strictly increasing generation order; see `refactor/CHECKPOINT-B.md` for the embedding contract.

- Checkpoint B now includes a portable per-connection deadline/recovery policy and a real four-peer ICE restart scenario: fresh credentials, retired-candidate filtering, preserved settings and media isolation. Production action dispatch/room integration remains open. Restart JSON separates negotiation, frame-check and ICE-state-confirmation timing; early evidence showed prompt frames but delayed state notification, so do not report the latter as a media outage. See `refactor/CHECKPOINT-B.md`.

- Real native media proofs now use separately trickled ICE through portable `IceCandidateHandoff`, with description readiness barriers, stale-generation rejection, bounded candidates and callback release on close/failure. Debug/Release media suites pass 22/22. Authenticated room integration, connection deadlines/restarts and STUN/NAT tests remain open; normal application media is unchanged. See `refactor/CHECKPOINT-B.md`.

- Checkpoint B host capture/membership coordinator is implemented: serialized bounded commands, operation/session IDs, stale rejection, priority queued cancellation, subscriber isolation and joined workers. Four-peer proof uses it; peer/signaling ownership and the shared application facade remain next. Debug/Release media suites pass 21/21, applications 10/10, headless Debug smoke 6/6 and Release regression 27/27. See `refactor/CHECKPOINT-B.md`. Before/after claims require matched evidence under `refactor/COMPARISON.md`; native handle-growth failure remains open.

- Backend v2 is authorized on `refactor/backend-v2`. Gate A passed its original native integration/build criterion on 2026-09-15; current verdict: `refactor/CLOSEOUT-A.md`. Native media suites pass 20/20 in Debug/Release, application suites 10/10, and the rebuilt Release package passes extracted CLI/UI/GUI smoke tests with developer paths removed. Reusable capture ownership, bounded viewer delivery, four-peer headless/rejoin proofs and Auto/Manual source/RTP settings are implemented; complete session-facade integration is Checkpoint B. Important open defect: closeout current-binary resource checks completed 100 full and 20 rapid-close cycles without crash/timeout but FAILED handle bounds (+76 and +10, limit 8). Earlier passing MTA resource runs are historical evidence, not a current no-leak guarantee. Keep the application MTA lease, capture-owner dispatcher and GraphicsCapture.dll pin; investigate resource lifetime before production cutover. Full hardware/latency acceptance remains in E, Qt/distribution obligations and actual Inno/fresh-machine checks in release preparation. Normal application media remains legacy until integration is validated. Do not keep adding B/D/E work to A; use PLAN.md, TODO.md and CLOSEOUT-A.md.

- `main` is synced to `origin/main` after the backend API split and UI process-adapter merge.
- The app builds with CMake debug/release presets and produces `ScreenShare.exe`.
- Reusable native engine modules now build through `ScreenShareCore`; runtime-backed Share/Watch execution and the concrete session facade build through `ScreenShareAPI`. `ScreenShare.exe` compiles CLI parsing and links that API, while `ScreenShareUi.exe` links the API directly.
- The reusable typed session API step is complete; active build work is now the revamped native UI on top of `ScreenShareAPI`.
- Normal/default CMake builds now also create portable zip packages.
- The app can also build optional `ScreenShareUi.exe` when Qt 6 Widgets and Svg are available.
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
- PR #99 `Secure Worker room signaling`: hidden app-generated room keys in secure room links, Durable Object live room state, static-candidate fanout fix, and clearer direct-UDP-blocked diagnostics.
- PR #100 `Add active room directory`: KV-backed active room summaries plus safe `GET /rooms` and `GET /rooms/:roomId/summary`, with Durable Objects still verified as the live source of truth.
- PR #102 `Add joinable room list with room access keys`: UI room list joins public Worker rooms without copied secret links; Durable Object keeps the hidden room access key.
- Recent backend/UI integration: `ScreenShareSession` emits typed diagnostic events for stream/audio media status, NAT status, access-code/password failures, room-open conflicts, and preview-close handling, replacing the remaining live-session stdout parsing in `ScreenShareUi`. Live Share/Watch starts now flow through typed session configs for Worker rooms, direct/Nearby targets, and manual invite fallback. Runtime control is typed around stream settings, with live resolution changes as the first implemented field.

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
- `backend/api/ScreenShareAPI.h`: public concrete `screenshare::ScreenShareSession` API facade used by the UI and intended for the CLI path as it gets thinner.
- `backend/api/ScreenShareAPI.cpp`: `ScreenShareSession` implementation using memory runtime control, typed Share/Watch runner entrypoints, and typed event/status translation.
- `backend/core/ScreenShareSession.h`: shared session data types and helpers used by the API, CLI, and UI.
- `backend/core/SessionCommand.*`: typed Share/Watch session config to engine-argument bridge still used for UI command previews/self-tests, not normal live session execution.
- `backend/core/SessionRuntimeControl.*`: shared stop/runtime stream-settings control interface. CLI runs use the file-backed implementation; `ScreenShareSession` uses the memory-backed implementation for stop/settings requests. Resolution is the first implemented live setting.
- `backend/runtime/ScreenShareRuntimeInternal.h`: private runtime bridge used by the session runtime; not a UI-facing API.
- `backend/runtime/ScreenShareRunContext.h`: shared run context for runtime control and captured output callbacks.
- `backend/runtime/ScreenShareRuntimeOptions.h`: shared `Options` model, runtime constants, and small option enums/target specs used while CLI parsing and runtime execution are being split.
- `backend/runtime/ScreenShareSessionOptions.*`: shared typed Share/Watch config-to-runtime-options conversion plus session/access-code validation and NAT target helpers. CLI presets and `ScreenShareSession` typed runs use this instead of duplicating typed setup logic in `ScreenShareCLI.cpp`.
- `backend/runtime/ScreenShareSessionRunner.h`: typed Share/Watch runner entrypoints used by the concrete session API; this is separate from the CLI app header.
- `backend/runtime/ScreenShareSessionRunner.cpp`: runtime-backed typed Share/Watch runner entrypoint implementation. It owns the typed-run report/log wrapper, then calls shared runtime execution directly.
- `backend/runtime/ScreenShareRuntimeExecution.cpp`: shared normal runtime execution for capture/send, receive/preview/audio playback, standalone audio capture, live signaling setup, adaptation policy, and typed Share/Watch execution entrypoints.
- `backend/runtime/ScreenShareRuntimeSupport.*`: shared session ID/fingerprint, stdout/stderr capture, saved-report zip, and argv support used by both CLI and typed runtime paths.
- `frontend/cli/ScreenShareCLI.*`: CLI parser/report wrapper compiled only into `ScreenShare.exe`. Normal CLI Share/Watch presets parse into typed session configs and use the same typed execution path as the UI/backend path; diagnostic-only CLI modes still parse into internal options directly and route through CLI-owned command dispatch.
- `frontend/ui/QtSessionBackend.*`: Qt-thread bridge over `screenshare::ScreenShareSession`. Live Share/Watch and normal display/audio-device discovery in the desktop UI no longer launch `ScreenShare.exe` as a child process.
- `assets/brand/` and `assets/ui/icons/`: first-pass logo and button icon SVG sources for the revamped UI.
  The Qt UI embeds the current mark/icons through `frontend/ui/resources.qrc` and links/packages QtSvg for SVG rendering.

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
  - optional `ScreenShareUi.exe` when Qt 6 Widgets and Svg are available.
  - dark-mode default with theme toggle.
  - Share/Watch presets, Start/Stop, command preview, live output, session/report controls.
  - portable zip includes Qt plugin folders and transitive runtime dependencies.
  - Live Share/Watch runs now go through `frontend/ui/QtSessionBackend.*` and `screenshare::ScreenShareSession`, so the UI calls the in-process session API on a worker thread instead of launching `ScreenShare.exe`.
  - Live Share/Watch now builds engine options directly from typed configs for Worker rooms, direct/Nearby targets, and manual invite fallback. Normal CLI Share/Watch presets produce the same typed configs; advanced diagnostic-only flags remain on the internal CLI options path until they become real app controls. Stop/runtime stream-settings controls route through `backend/core/SessionRuntimeControl.*` with file-backed control for CLI runs and memory-backed control for UI runs.
  - `ScreenShareSession` now translates live telemetry into typed `SessionEvent` snapshots: Share viewer rows use typed viewer status, Watch/Share live indicators use typed activity, and NAT hints, access-code/password failures, room-open conflicts, and preview-close handling flow through typed events instead of UI-side stdout parsing.
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
  - v0.2.3 gives every signaling viewer a stable-ID send lane over the shared NAT-bound socket, with independent queue/pacing/drop/error state, GOP-aware recovery, fresh-feedback state, and host disconnect.
  - Global bitrate reduction now requires agreement from all fresh viewers; one degraded outlier cannot pull down healthy viewers.
  - `scripts/run-multiviewer-harness.ps1` covers healthy, loss/jitter, slow-consumer, late-join, leave/rejoin, and unreachable scenarios and writes per-viewer CSV/assertions.
- Live stream controls direction:
  - User reports native/2K looks sharp, but 1080p stretched to a 2K preview is still blurry after sharper scaling. Do not assume bitrate is the cause; after the revamped UI, investigate whether live resolution changes accidentally alter bitrate, encoder quality, chroma/subsampling behavior, scaler path, or preview upscale behavior.
  - After the revamped UI, add a live settings panel that can change parameters without restarting the room: Quality/Bitrate, FPS/adaptive FPS, resolution, encoder preference/preset, audio device, and audio mute.
  - Adaptive FPS should share the same typed runtime stream-settings control model as bitrate/resolution and reduce frame rate only under sustained pressure.
- Signaling direction:
  - First backend lives under `signaling-worker/`.
  - It is Cloudflare Worker + Durable Objects for live room membership, peer UDP candidates, heartbeat, cleanup, and the browseable active-room directory.
  - Durable Object room state replaced KV room storage after real reports showed asymmetric visibility (`Watch` saw `Share`, while `Share` stayed at zero targets) during rapid join/poll cycles.
  - It must not relay media or store plaintext room passwords/user-visible access codes.
  - For no-password public rooms, the Durable Object generates and stores a random room access key. Native clients use it as the hidden UDP access code so users get encrypted UDP media without seeing an access-code field. This is encryption, not private access control.
  - Optional room passwords are verified by the Worker over HTTPS with a salted verifier stored in the Durable Object, then the native app also mixes the typed password into the hidden room access key before UDP key derivation.
  - Native C++ diagnostic integration started with `backend/transport/SignalingClient.*` plus `--signal-health`, `--signal-join`, `--signal-peers`, `--signal-heartbeat`, and `--signal-leave`.
  - Live Share/Watch CLI integration is in progress: `--watch PORT --signal-room ROOM` publishes the watcher candidate and turns returned peers into NAT probe targets; `--share-room PORT --signal-room ROOM` publishes the sharer candidate and turns returned peers into UDP send targets. `--signal-server URL` overrides the built-in Worker.
  - Runtime live signaling uses authenticated heartbeat/peer reads plus WebSocket events; it re-announces only when local metadata changes. Share can start before Watch and wait for peers; Watch can add newly discovered room peers as NAT probe targets; Share can add newly discovered watcher candidates to the active sender socket.
  - The UI default Internet path now uses the built-in Worker `https://screenshare-signaling.bit-yeet.workers.dev`: Share has Room ID, friendly Name, optional Password, and Port, and copies a short `screenshare-room-v1;room=...` link; Watch can pick an active room from `GET /rooms` or paste the link. The engine receives the room access key from signaling during join and uses it as the hidden UDP access code, optionally mixed with the room password.
  - `GET /rooms` returns directory Durable Object-backed active-room summaries for browse/debug views, and verifies each listed room against its room Durable Object so stale entries do not stay visible in the API. `GET /rooms/:roomId/summary` returns one safe summary or `null`.
  - Live signaling publishes a `host` local candidate alongside the `srflx` STUN candidate when available, so same-PC and same-LAN room tests can use the direct local path instead of relying on router hairpin behavior.
  - Manual NAT invite fields still exist behind a fallback checkbox.
  - `ScreenShareUi` runs `windeployqt` through `cmake/RunWindeployQt.cmake`; the script always verifies/copies the current Qt DLLs/plugins and resolved MinGW runtime deps so the release UI does not keep stale mismatched Qt files.
  - Keep the UI runtime consistently UCRT (`C:/msys64/ucrt64/bin`); mixing `mingw64` and `ucrt64` Qt/ICU/libstdc++ DLLs causes Windows entry-point loader errors before the app starts.
  - Remaining signaling TODO is real multi-computer/multi-viewer validation across separate NATs.

- Receiver recovery follow-up: v2 presentation now catches typed DXGI device failures, releases resources on the window thread, drops frames during 250 ms backoff and allows three rebuilds per presenter lifetime. Fourth failure is terminal. Debug/Release policy/resource-recreation proofs pass with injected failures; actual driver removal and capture/encoder recovery remain open. Hardware/audio proof suites now pass 13 tests each; application suites pass 8 each. Desktop GPU proof requires `PresentationRecoveryTest --gpu`. Foundation committed as `4ca8318`; continue substantial logical commits.

- Capture/encoder recovery follow-up: original-item WGC resource reconstruction and shared hardware-session retirement are implemented and tested. Retired-device native frames drop without readback; two viewers resume software IDRs from a replacement device. Live WGC rebuild/closure passes three cycles per Debug/Release; proof suites 13/13 and application suites 8/8. Automatic capture recovery, bounded coordinator retries and repeated device generations remain pending, as does actual driver-removal testing. Continue with those integration boundaries before claiming full recovery or passing Gate A.

- Automatic proof recovery milestone: LiveCaptureSource now handles typed device loss with three rebuilds, 250 ms cancellable backoff, first-frame deadline and device generation counters. Every D3dVideoDevice owns retirement; old/cached frames from replacement devices are rejected, and two encoders resume IDRs across three owners. Debug/Release suites pass 14/14; automatic capture and live WebRTC desktop proofs pass. One Release LiveCaptureTest repeat run failed around second-cycle shutdown without exception text; standalone and cdb reruns passed. Investigate this intermittent failure next; do not mark lifecycle or Gate A complete. Production coordinator/callback-generation barriers and actual driver removal remain unverified.

- Larger build-delivery milestone: both content-addressed WebRTC SDKs exported with full file hashes, compiler builtins and dependency notices; CMake accepts relocated SDKs and validates ABI/compiler/MSVC/SDK identity. Fresh proof suites pass 14/14, fresh application suites 9/9 after adding the UI self-test. Native Release zip passes extracted CLI/self-test/Windows GUI startup with developer PATH removed and package-local plugins confirmed. Fixed MSVC runtime staging, native Qt plugin layout/Debug selection, and Qt application initialization in self-test. Twenty debugger plus twenty normal Release capture cycles passed, but the prior unexplained lifecycle failure remains open. See latest BUILD.md/CHECKPOINT-A.md sections. Next: remaining Gate A protocol fixtures, external baseline, lifecycle investigation, installer/distribution evidence. Normal application media remains legacy.

- Application COM lifetime milestone (2026-09-15): remaining retained events traced to COM OXID/apartment remoting. Scoped WindowsMediaRuntime in GUI/CLI/proof keeps MTA support alive across sessions and joined workers; create after QApplication/UI STA, destroy after sessions. Full Release 100 cycles passes (339.22 s, median handles 380 to 379); Debug rapid-close and fresh-owner 100 cycles each pass at 330 to 330. Debug/Release media 16/16, app 10/10, GUI startup and 8 stress-runner tests pass. API embedders must retain the same application owner. No global COM static; capture dispatch and system-module pin remain. Complete resource teardown, actual driver removal, production v2 coordinator, external gaming latency and distribution remain pending; normal media remains legacy. See latest refactor/CHECKPOINT-A.md.
