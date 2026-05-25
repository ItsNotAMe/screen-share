# UI Notes

## Current UI Direction

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

## Implementation Notes

- `ScreenShareUi.exe` lives beside `ScreenShare.exe`; live Share/Watch sessions use `src/ui/QtSessionBackend.*` over `src/app/AppSessionBackend.*`.
- `ScreenShareUi.exe` links `ScreenShareAppRunner`, which includes the app runner and app session backend. Short helper diagnostics can still invoke `ScreenShare.exe` when useful.
- `AppSessionBackend` converts live engine telemetry into typed `SessionEvent` snapshots. The UI uses those events for Share viewer rows, the Live/Disconnected indicator, NAT hints, access-code/password failures, room-open conflicts, and preview-close stop handling instead of parsing those fields from stdout itself.
- Live Share/Watch launch arguments are built through `src/core/SessionCommand.*` from typed session configs for Worker rooms, direct/Nearby targets, and manual invite fallback. The UI should not own live-session argument assembly.
- CLI stop/control files and UI memory controls now flow through `src/core/SessionRuntimeControl.*`; the runtime API is typed around stream settings, with resolution as the first implemented live setting.
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
- The display chooser should show user-readable monitor metadata, such as display index, output name, resolution, position, and primary status, while still passing the numeric `--display N` value to the engine. Prefer the engine's `--list` output over Qt screen ordering so the shown index matches capture exactly; use Qt only as a startup/fallback list.
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
