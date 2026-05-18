# UI Notes

## Current UI Direction

- Use Qt Widgets for the first real desktop UI.
- Keep the first phase as a small control shell over the existing `ScreenShare.exe` engine.
- Visual direction: modern, simple, practical. Avoid dense advanced settings until the core flow feels good.
- Dark mode is the default, with a visible light/dark toggle in the header.
- Form rows should use explicit layout margins; Qt stylesheet padding on `QGroupBox` can cause first-paint overlap until resize.
- Avoid `QGroupBox` for the option sections; use plain `QFrame#Panel` sections with a title label plus form grid.
- Keep ordinary labels transparent so global dark/light backgrounds do not paint ugly strips behind text.
- UI Stop should request a graceful engine exit with a hidden stop-file signal before force-killing the process; otherwise Qt reports sender termination as a crash and saved reports may not flush.
- Primary tabs:
  - Share screen: one Connection panel with tab-like buttons for Nearby, Internet, and Manual; then friendly display chooser, FPS, resolution, and system-output audio device.
  - Watch room: one Connection panel with the listen port above tab-like buttons for Nearby (LAN discoverable) and Internet (invite exchange), then mute, volume, preview latency.
  - Watch's Nearby and Internet tabs use the same `ModeBar` + `ModeButton` styling as Share's so the connection-method vocabulary is identical on both sides.
- Reports should stay easy: enabled by default with `sender-report.zip` / `receiver-report.zip`.
- The window is a single-column settings shell now; the Command preview and Output console were removed because they pushed every settings row sideways and bloated the chrome. Engine stdout is still captured for NAT-status parsing and the saved-report zip; routine stdout from the UI is mirrored to qDebug so it can be tailed from a terminal build.
- Streaming indicator: header pill has four states. `○ Idle` (gray, no pulse) when nothing runs; `◔ Connecting` (amber, pulsing) when the engine is running but no peer has been observed yet; `● Live` (green, pulsing) while peer activity is actively increasing; `× Disconnected` (red, no pulse) after a previously live peer stops responding. Do not make Live sticky: Share watches increasing `udp_feedback_packets` / `udp_nat_probe_packets`, Watch watches increasing `completed_frames` / `payload_bytes` / `h264_decoded_frames` / `audio_packets`, and a timer drops to Disconnected after a short quiet period.
- Share status depends on receiver feedback reaching the sender. The media UDP sockets disable Windows `SIO_UDP_CONNRESET` best-effort so starting Share before Watch, or restarting Watch while Share keeps running, does not leave the sender feedback path stuck after an ICMP "port unreachable" reset.
- Mode switching uses a small `PageStack` (plain `QWidget` + `QVBoxLayout` + show/hide), not `QStackedWidget`. `QStackedLayout::minimumSize()` returns the max across all pages, which leaks past any sizeHint override and forces the column to allocate the tallest page's height to every page — that was the source of the empty gap between the option stack and the Security panel.
- Security UX: generate/copy one access code from the Security panel and require an explicit plaintext checkbox before starting without a code.
- UI session IDs are automatic; keep manual `--session` as a CLI/reporting diagnostic escape hatch, not a normal UI field.

## Implementation Notes

- `ScreenShareUi.exe` lives beside `ScreenShare.exe` and launches it with `QProcess`.
- `--share` is the sender preset, so it already enables system audio, adaptive bitrate, and adaptive resolution.
- Share audio uses Windows' default output unless `--audio-device-id` is supplied. The UI should expose a sender-side output-device picker because virtual mixers can make the Windows default endpoint differ from the device that actually contains app audio.
- `--watch` is the receiver preset, so it already enables preview, audio playback, and default A/V sync.
- If `--share` cannot capture audio, automatic receiver sync should still show video; the engine falls back to `video_only_no_audio` after 30 video frames.
- Watch mode has LAN discoverable enabled by default and adds `--lan-advertise`.
- Share mode runs quiet background target discovery into the Connection panel's Nearby Devices list and has a manual Refresh button for visible discovery output.
- The Nearby Devices list combines true LAN receiver advertisements with optional Tailscale peers from `tailscale status --json` when the local Tailscale CLI is installed.
- Tailscale peers are quick targets at the current Share port only; they do not prove that Watch is running or advertise access-code/security metadata.
- Internet/NAT fields are a transitional bridge over the CLI invite flow: the UI can create/copy this side's invite using `--make-invite`, paste/extract a friend invite from clipboard text, Share accepts the Watch invite plus this side's invite, and Watch accepts the Share invite so it can send probes from the receive socket.
- The UI invite creation uses the shared Access code or explicit plaintext choice. Invite blobs are copied to the clipboard, but the raw access code still has to be shared separately.
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
- Tailscale/mesh VPN peers are already usable by manual IP in the Share address field. The Tailscale peer picker is separate from UDP LAN broadcast discovery because mesh VPNs usually do not forward broadcast.
- Discovery output never includes a raw access code. For encrypted receivers, the UI compares the typed access code with the advertised access-code fingerprint and warns on mismatch.
- Wrong discovered-receiver access codes should clear the Access code field, disable plaintext, and focus the field so the user can retry immediately.
- Share should warn before starting when the target is localhost/loopback; that usually means the sender is about to send to their own computer instead of the remote Watch computer.
- Use `--self-test` on the UI for automated smoke checks; it verifies that the engine executable is beside the UI.
- Packaging must include Qt DLLs, plugin folders, and their transitive MinGW/UCRT dependencies.
