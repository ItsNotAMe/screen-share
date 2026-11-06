# UI Notes

## Current UI Direction

- Shared-source capture failures explain automatic retry while video continues.
  Other audio modes retain their manual retry guidance. Keep the selected app
  authoritative and allow source changes/mute during recovery.

- Viewer output failures recover automatically with capped retries while video
  continues. Explain automatic retry in playback settings and keep manual Retry
  and device selection usable. Restore Retry with QFormLayout::setRowVisible,
  matching its initial row hide; showing only the button leaves its old default
  geometry over the other controls. Check full bounds and non-overlap on failure.

- Home/Join directory rows show occupied/total viewer places. Use readable Full
  or Wait… labels without an action icon when joining is unavailable; provide
  full/host-reconnecting/list-unavailable reasons in tooltips and accessibility.
  Restore Join and its icon from the next authoritative available snapshot.
  Preserve row identity and action geometry through these updates.

- Create/live source pickers share title search. Filter cards in place without
  restarting previews, retain query through tabs/refresh and keep a valid visible
  selection. No-match/unavailable feedback is centered and sharing is disabled.
  Focus search on open; Enter moves to matching cards without confirming a share.
  Use up to three columns with readable cells (210px minimum), falling back to
  two/one in compact containers. Base wrapping on widget width with scrollbar
  space reserved, and retain selected-card identity during resize.

- Pending Join shows neutral progress, disables the Join action and uses the
  Paste slot for Cancel. Preserve the invite and keep action/list geometry stable.
  Home cancels and waits for cleanup before navigating; ignore late success or
  password failures after cancellation. Measure action states after style polish.

- Explain authoritative remote room closure with the existing toast after
  session cleanup and return Home/Join. Distinguish host departure, expiry and
  service shutdown; keep local Leave/host Stop quiet and removal's persistent
  explanation separate. Never infer remote closure from Stopped alone.

- Join failures preserve the invite and explain full/closed/missing rooms, rate
  limits and uncertain outcomes using the shared RoomEntryError guidance.
  Host creation uses service-capacity guidance for the full response. Keep
  password prompts exclusive to validated authorization failures.
  Session windows must also render the typed terminal cause on every failed
  status push, using active-room recovery wording after admission. Avoid
  replacing detailed guidance with a generic failure or keeping stale action errors.

- Validate Create room names and passwords before allocating a session, using
  the protocol's normalization and admission's shared password validator. Keep
  the draft and focus the invalid field. Validate Join passwords inside the
  existing popup so users can correct them without losing the invite or another
  admission attempt. Unicode character limits and UTF-8 password limits differ;
  explain long passwords without silently trimming or truncating them.
  Preserve form-field minimum sizes through both the details layout and the
  responsive columns; at compact heights, scroll instead of overlapping cards.

- Normal startup remembers windowed size/position and maximized state. Cache
  placement in memory while moving; freeze/save on the first Close before room
  drain changes geometry. Fullscreen and minimization stay temporary. Decode Qt
  saved geometry on an unshown helper, then use the existing native maximize
  path after first show; direct hidden maximized restore breaks the custom chrome.
  Keep diagnostic launches and test profiles independent of user settings.

- Viewers clear stale pictures for fresh host notices of paused video, minimized
  or unavailable sources, and capture recovery. Keep Resuming video until an
  actual decoded frame reaches the UI. Notices use the existing video status
  surface and accessible description. Clear held input before clearing mapping;
  gate remote input until fresh video while preserving grants and focus policy.
  Game mouse must be captured again after the video returns.
  Use the existing Controls hint to explain paused remote input persistently:
  wait for fresh video first, then offer click-video recovery for a focus/mode
  pause. Keep shortcut reminders and use the actual forwarding gates/grants;
  permission alone does not mean input is currently forwarding.

- Hosts can remove a viewer from that viewer's row after a named confirmation.
  Keep removal icon-only, using user-minus beside the three input icons on one
  line. Elide long names, retaining their full text in tooltips and accessibility.
  Reuse SessionPopup, focus Cancel and preserve identity/focus during room pushes.
  Removal targets peer ID, not nickname; handle departure while confirmation is
  open. Other viewers stay connected. Removed viewers return to Join with an
  explanation. Removal ends a membership and does not ban future admissions.

- Host Mute audio and source-picker audio changes work before a viewer arrives.
  Show Audio muted in the summary and synchronize the speaker icon. Share audio
  restores the exact prior selection. An explicit mute cancels pending automatic
  source edits; an idle audio choice saves configuration without opening devices.

- Local create/host monitoring previews run at five FPS and fit on the GPU before
  BGRA readback (640 × 360; chooser thumbnails 320 × 180). Preserve source aspect
  ratio and original size metadata. Preview waits must honor worker interruption.
- Home and Join preserve room rows/focus during directory pushes. Access changes
  update both the icon and badge styling; no-match search shows a Clear search
  action and differs from a genuinely empty directory.
- Heavy room diagnostic snapshots and GUI status copies run at 100ms cadence;
  frame handoff and media/control retirement keep their independent short tick.

- Create room retains a selected-source live preview and opens the same enlarged
  SourcePickerDialog used by the host room. Keep explicit confirmation/cancel,
  current selection on cancellation, full-title tooltips and keyboard focus return.
  Closed shared windows keep the room alive and show Change source guidance.


- Host/viewer Details opens to a readable Overview with labelled connection, video and audio measurements. The host viewer picker and status note stay visible during scrolling; long diagnostics and the per-viewer table live under Advanced. Missing/stale values are explicit, RTT is network round trip, and host encoder output is distinct from viewer-reported decode.
- Details keeps Open log folder and Save report reachable below both tabs. Save
  report opens an asynchronous platform Save As dialog, with a role/UTC JSON
  suggestion in Documents or the configured report path. Remember the selected
  destination for the session, keep overwrite confirmation, preserve feedback on
  cancellation and dismiss the dialog on session close. The folder action always
  remains enabled, creates its directory on demand and opens the log folder
  independently of report-export destination or save success.
  Theme Qt fallback file lists/sidebar, selected rows, navigation buttons and
  horizontal scrollbars explicitly; inherited pale text on a native light base
  is unreadable. Verify with a populated folder rather than an empty picker.
- Room viewer controls are icon-only Mouse/Keyboard/Controller buttons, all off initially. Clicking requests immediately; amber means requested and teal means granted. Keep tooltips and accessible names. Requests accumulate without revoking existing grants.
- Host pending icons pulse amber (respect Windows animation preference); clicking opens Allow/Deny for that control only. Requests gather per viewer, while approval and denial remain independent. Denial preserves existing grants and held input. Viewer selection uses fill colors without borders; focus indication appears only for keyboard navigation.
- Use Qt Widgets for the first real desktop UI.
- The UI should call the native backend in-process for live Share/Watch sessions instead of being a launcher shell.
- Visual direction: modern, simple, practical. Avoid dense advanced settings until the core flow feels good.
- Dark mode is the default, with a visible light/dark toggle in the header.
- Form rows should use explicit layout margins; Qt stylesheet padding on `QGroupBox` can cause first-paint overlap until resize.
- Avoid `QGroupBox` for the option sections; use plain `QFrame#Panel` sections with a title label plus form grid.
- Keep ordinary labels transparent so global dark/light backgrounds do not paint ugly strips behind text.
- UI Stop should request a graceful backend exit through memory runtime control.
- Primary tabs:
  - Create room: one Room panel with tab-like buttons for Nearby, Internet, and Manual; it opens on Internet by default, then friendly display chooser, FPS, resolution, and system-output audio device.
  - Join room: one Room panel with the listen port above tab-like buttons for Nearby (LAN discoverable) and Internet (paste room invite); it opens on Internet by default, then mute, volume, preview latency.
  - Watch's Nearby and Internet tabs use the same `ModeBar` + `ModeButton` styling as Share's so the connection-method vocabulary is identical on both sides.
- Reports should stay easy: enabled by default with `sender-report.zip` / `receiver-report.zip`.
- The window is a single-column settings shell now; the Command preview and Output console were removed because they pushed every settings row sideways and bloated the chrome. Backend stdout is still captured for saved-report logs; routine stdout from the UI is mirrored to qDebug so it can be tailed from a terminal build.
- Streaming indicator: header pill has four states. `○ Idle` (gray, no pulse) when nothing runs; `◔ Connecting` (amber, pulsing) when the engine is running but no peer has been observed yet; `● Live` (green, pulsing) while typed backend status reports active viewer/receiver progress; `× Disconnected` (red, no pulse) after a previously live peer stops responding. Do not make Live sticky: backend status marks activity from receiver feedback and received media counters, then a UI timer drops to Disconnected after a quiet period.
- Share status depends on receiver feedback reaching the sender. The media UDP sockets disable Windows `SIO_UDP_CONNRESET` best-effort so starting Share before Watch, or restarting Watch while Share keeps running, does not leave the sender feedback path stuck after an ICMP "port unreachable" reset.
- Sender feedback receive drains through NAT probes and invalid packets instead of stopping at the first non-feedback datagram. Otherwise queued probes can make old feedback leak out slowly and keep the Share UI falsely Live long after Watch exits.
- Mode switching uses a small `PageStack` (plain `QWidget` + `QVBoxLayout` + show/hide), not `QStackedWidget`. `QStackedLayout::minimumSize()` returns the max across all pages, which leaks past any sizeHint override and forces the column to allocate the tallest page's height to every page — that was the source of the empty gap between the option stack and the Security panel.
- Security UX direction: normal public room sharing should be encrypted by default with an automatic random room access key from signaling. Do not expose "access code" as the default user chore; room password is an optional extra lock for Worker rooms.
- UI session IDs are automatic; keep manual `--session` as a CLI/reporting diagnostic escape hatch, not a normal UI field.
- Stage-2 UI code should not grow as one huge file. Split it into logical units: app shell/navigation, shared widgets/styles, room flow screens, active session screens, and small data/adapters where useful.
- Keep UI code simple and direct. Add abstractions only when they remove real duplication or make screens easier to read.
- Active Share/Watch layouts stay compact when maximized: Share centers content within a bounded reading width and caps dashboard-card heights; Watch keeps its volume controls grouped instead of stretching their internal spacing. Stream-fullscreen exit restores native geometry before revealing normal controls and forces a final native redraw to prevent stale footer ghosts.
- When each new screen first runs, pause and ask the user for a screenshot so the screen can be visually validated before moving to the next screen.

## Implementation Notes

- `ScreenShareUi.exe` lives beside `ScreenShare.exe`; live Share/Watch sessions use `frontend/ui/QtSessionBackend.*` over the concrete `screenshare::ScreenShareSession` API.
- `ScreenShareUi.exe` links `ScreenShareAPI`, which includes the concrete session API and runtime-backed Share/Watch execution without CLI parsing. Short helper diagnostics can still invoke `ScreenShare.exe` when useful.
- The UI-facing session API is `backend/api/ScreenShareAPI.h`; `backend/runtime/ScreenShareRuntimeInternal.h` is private runtime plumbing shared with the current CLI implementation.
- Runtime-owned saved-report and stdout/stderr capture support lives in `backend/runtime/ScreenShareRuntimeSupport.*`, so typed UI sessions and CLI sessions now use the same report wrapper.
- `ScreenShareSession` converts live engine telemetry into typed `SessionEvent` snapshots. The UI uses those events for Share viewer rows, the Live/Disconnected indicator, NAT hints, access-code/password failures, room-open conflicts, and preview-close stop handling instead of parsing those fields from stdout itself.
- `SessionStatus::stream` carries typed stream telemetry for the future Active Share/Watch Session screens: output resolution, source resolution when available, FPS, bitrate, adaptation state, stream queue/drops, and UDP queue delay.
- `SessionStatus::audio` carries typed audio telemetry for future active-session panels: capture/receive counters, codec/format, UDP audio transport, playback queue/render state, mute/volume, and A/V sync status.
- Live Share/Watch sessions build engine options directly from typed configs for Worker rooms, direct/Nearby targets, and manual invite fallback. `backend/core/SessionCommand.*` remains only for command previews/self-tests while that UI surface still exists.
- CLI stop/control files and UI memory controls now flow through `backend/core/SessionRuntimeControl.*`; the runtime API is typed around stream settings, with resolution as the first implemented live setting.
- `--share` is the sender preset, so it already enables system audio, adaptive bitrate, and adaptive resolution.
- Share audio uses Windows' default output unless `--audio-device-id` is supplied. The UI should expose a sender-side output-device picker because virtual mixers can make the Windows default endpoint differ from the device that actually contains app audio.
- `--watch` is the receiver preset, so it already enables preview, audio playback, and default A/V sync.
- If `--share` cannot capture audio, automatic receiver sync should still show video; the engine falls back to `video_only_no_audio` after 30 video frames.
- Watch mode has LAN discoverable enabled by default and adds `--lan-advertise`.
- Share mode runs quiet background target discovery into the Connection panel's Nearby Devices list and has a manual Refresh button for visible discovery output.
- The Nearby Devices list combines true LAN receiver advertisements with optional Tailscale peers from `tailscale status --json` when the local Tailscale CLI is installed.
- Tailscale peers are quick targets at the current Share port only; they do not prove that Watch is running or advertise access-code/security metadata.
- Internet/NAT fields are a transitional bridge over the CLI invite flow: Create room generates/copies one sharer room invite, Join room pastes that invite and sends probes from the receive socket, and Watch can also create/copy a My invite response. Multiple watchers can paste the same room invite; Share fans out to every valid watcher probe it sees. If strict NAT blocks some probes, paste those watchers' My invite responses into Share's Watcher invites list; the engine sends outward to those watcher endpoints through the same sender socket/local port.
- Worker signaling is now the default Internet path in the UI. The app uses the built-in Worker URL `https://screenshare-signaling.bit-yeet.workers.dev`; users only see Room and local Port controls, and copied room links are short `screenshare-room-v1;room=...` strings. The UI does not ask users to paste the Worker URL.
- Join room's Internet tab shows a lightweight active room list from `GET /rooms`. It uses Qt Network directly, refreshes in the background, and shows friendly room name, room ID, peer count, lock status, and freshness. Selecting an open room is enough to start because the engine receives the room access key from signaling during join. Selecting a locked room should focus the password field.
- Watch should keep its own UDP listen port when pasting a room link. The Worker exchanges the actual per-peer UDP ports through candidates.
- The old NAT invite widgets remain available behind "Manual invite fallback" on the Internet tab. Keep them tucked away unless the Worker room flow fails or the user explicitly needs manual fallback.
- Worker room sharing uses an automatic random room access key returned by signaling. Optional Worker room passwords are passed to the engine as `--signal-room-password`, redacted in previews/log command lines, verified by the Worker over HTTPS, and mixed into the UDP key locally. Manual invite fallback still uses the visible Access code or explicit plaintext choice.
- The Connection panel has one compact status hint for the active method. Keep it short and stateful, not long tutorial prose.
- The display chooser should show user-readable monitor metadata, such as display index, output name, resolution, position, and primary status, while still passing the numeric `--display N` value to the engine. Use the typed backend display API so the shown index matches capture exactly; use Qt screens only as a startup/fallback list.
- While Share/Watch is running with NAT invite mode, the UI parses engine output fields like `nat_status=` and `nat_hint=` and replaces the Internet hint with a short live status such as probing, probe seen, connected, receiving, or rejected.
- Receiver preview clears to a black frame after a previously live stream goes quiet for a couple seconds, so a stale last frame does not look like an active share.
- Selecting an encrypted discovered receiver fills address/port and focuses the main Access code field if it is empty. Do not use a separate password dialog here; the field is the single source of truth.
- Receiver discovery auto-refresh should stay relaxed, currently 15 seconds, so the UI does not feel noisy.
- Discovered receivers may answer on both loopback and LAN when testing locally; prefer the non-loopback address for the same advertised fingerprint/port.
- Spin boxes and combo boxes should ignore mouse wheel events so scrolling the settings pane does not accidentally change parameters.
- Form inputs should use `Qt::StrongFocus`, not wheel focus, so scrolling the settings pane does not focus/highlight the field under the pointer.
- Combo boxes can contain long display/audio labels; keep their size policy shrink-friendly and rely on elided text plus tooltips so the Share settings pane does not clip horizontally.
- Product direction: the final room model should have the sharer create/host a room. The current Targets list is a transitional UI over today's Watch-advertises/Share-connects transport plus Tailscale quick targets.
- Multi-viewer UI direction: avoid a separate "extra target" box. Nearby supports multi-select from the discovered/Tailscale list, Manual accepts multiple comma/space-separated targets in the main Targets field, and Internet uses one shared room invite for multiple watchers.
- The main UI should not expose per-watcher sharer-local invite rows. The current room model is one shared sharer room invite plus optional watcher response invites; future room work should add per-viewer connection state after real testing shows what status users need.
- Tailscale/mesh VPN peers are already usable by manual IP in the Share address field. The Tailscale peer picker is separate from UDP LAN broadcast discovery because mesh VPNs usually do not forward broadcast.
- Discovery output never includes a raw access code. For encrypted receivers, the UI compares the typed access code with the advertised access-code fingerprint and warns on mismatch.
- Wrong discovered-receiver access codes should clear the Access code field, disable plaintext, and focus the field so the user can retry immediately.
- Runtime access-code failures should use the same retry path: parse engine output for encrypted invite decrypt failures, invite fingerprint mismatches, and increasing `access_rejected_datagrams` / `crypto_rejected_datagrams` counters, then warn once and clear/focus the Access code field.
- Share should warn before starting when the target is localhost/loopback; that usually means the sender is about to send to their own computer instead of the remote Watch computer.
- Use `--self-test` on the UI for automated smoke checks; it verifies that the engine executable is beside the UI.
- Packaging must include Qt DLLs, plugin folders, and their transitive MinGW/UCRT dependencies.
