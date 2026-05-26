# TODO

## Your Validation

Only you can fully validate these because they need real machines/networks.

- Worker room validation already tested: open room across two LANs, and locked/password room locally.
- Remaining Worker room validation: locked/password room across two LANs, watcher joins an idle room before Share resumes, sharer reconnect, and two or more watchers with WebSocket peer notifications plus HTTP fallback.
- Mid-session stream settings validation: while sharing, switch Resolution between Auto and explicit tiers at or below the display size and confirm Watch keeps receiving after each restart.

## Build Work

1. Finish one reusable typed session API for both CLI and UI.
   - [x] Run UI live Share/Watch sessions in-process on a worker thread.
   - [x] Use typed launch configs for Worker rooms, direct/Nearby targets, and manual invite fallback.
   - [x] Use typed runtime stream-settings control, with resolution as the first implemented live setting.
   - [x] Emit typed Share viewer status, Watch media activity, stream/audio media status, NAT hints, access-code/password failures, room-open conflicts, and preview-close handling.
   - [x] Move display listing behind a backend API instead of a helper CLI process.
   - [x] Move audio device listing behind a backend API instead of a helper CLI process.
   - [x] Define the typed session API surface around Start Share, Start Watch, Stop, runtime settings, status snapshots, typed events, display discovery, and audio-device discovery.
   - [x] Move normal Share/Watch execution behind typed API methods instead of the current `RunScreenShareCli(argv)` bridge.
   - [x] Make the concrete session API call typed Share/Watch runner entrypoints directly, without rebuilding CLI arguments internally.
   - [x] Replace the typed Share/Watch runner entrypoints' temporary internal argument bridge with direct options/session execution.
   - [x] Make normal CLI Share/Watch presets parse into the same typed configs and execution path as the UI.
   - [x] Split typed Share/Watch session runner declarations away from the CLI app header.
   - [x] Rename the shared app-runner CMake target to `ScreenShareSessionRuntime`.
   - [x] Move typed Share/Watch runner entrypoint implementation into `ScreenShareSessionRunner.cpp`.
   - [x] Replace the old `AppSessionBackend` interface adapter with a concrete `ScreenShareSession` API facade.
   - [x] Move CLI source into `src/cli` and private session runner plumbing into `src/runtime`, removing the vague `src/app` folder.
   - [ ] Promote remaining advanced CLI-only Share/Watch diagnostic flags into typed configs only when they become normal app controls.
   - [ ] Continue reshaping CMake targets so `ScreenShareCore` owns the reusable session API, `ScreenShare.exe` is the thin CLI app, and `ScreenShareUi.exe` links the same API directly instead of CLI parsing internals.
   - [x] Move session IDs/fingerprints, report zip writing, stdout/stderr capture, and typed-run report wrapping into `src/runtime`.
   - [x] Move the shared runtime `Options` model, option enums, target specs, and runtime constants into `src/runtime`.
   - [x] Move typed Share/Watch config-to-runtime-options conversion and shared NAT/session validation helpers into `src/runtime`.
   - [ ] Move remaining reusable Share/Watch runtime code out of `ScreenShareCLI.cpp` so `ScreenShareSessionRuntime` stops carrying CLI parsing internals.
   - [ ] Shrink `ScreenShareCLI.cpp` into CLI parsing, diagnostics, and command dispatch; move reusable Share/Watch runners into focused API/core files.
   - [x] Rename `ISessionObserver` to `ISessionEventSink` so event delivery reads as a simple session event sink/callback, not a vague pattern name.
   - [ ] Keep short helper diagnostics only where they are genuinely diagnostic, not normal UI data paths.
   - [ ] Add any remaining typed setup/session state needed by the Active Share/Watch Session screens.
2. Build the stage-2 native UI on top of the backend/core API.
   - [ ] Replace the launcher-style first screen with the saved room-flow draft.
   - [ ] Simplify Create room around the Worker room model, with no default Nearby/Manual tabs.
   - [ ] Simplify Join room around active rooms and pasted room links, with no default Nearby/Manual tabs.
   - [ ] Add active Share Session screen using typed stream/audio/viewer/session status.
   - [ ] Add active Watch Session screen using typed stream/audio/session status.
   - [ ] Add room settings side panel or popup for advanced fields.
   - [ ] Wire the saved SVG logo and button icons into the revamped UI surfaces.
   - [ ] Keep reports enabled and easy to find from active sessions.
3. After the revamped UI, investigate and fix lower-than-native resolution blur.
   - [ ] Confirm whether changing to 1080p changes bitrate or encoder quality unexpectedly.
   - [ ] Compare sender capture/scaler output against receiver decoded output.
   - [ ] Check chroma/subsampling and H.264 padding behavior for downscaled tiers.
   - [ ] Check preview upscale filtering from 1080p to 2K.
   - [ ] Fix the actual blurry path without hurting native/2K sharpness.
4. After the revamped UI, add live stream settings.
   - [ ] Add host controls for Quality/Bitrate.
   - [ ] Add host controls for FPS and adaptive FPS.
   - [ ] Add host controls for Resolution, including Auto and explicit tiers at or below display size.
   - [ ] Add host controls for encoder preference/preset.
   - [ ] Add host controls for audio device.
   - [ ] Add host-side outgoing audio mute.
   - [ ] Extend runtime settings beyond resolution as each engine control becomes live-safe.
   - [ ] Make Auto modes adapt bitrate, resolution, and FPS only under sustained pressure.
5. After the revamped UI, add application sharing.
   - [ ] Let the host choose a specific application/window video source.
   - [ ] Capture matching application audio where Windows allows it.
   - [ ] Make fallback behavior clear when per-app audio is unavailable.
   - [ ] Keep whole-display/system-audio sharing as the simple default.
6. Add better user-facing diagnostics when reports show the need.
   - [ ] Surface common setup mistakes in the UI instead of only logs.
   - [ ] Improve sync/network state wording on active session screens.
   - [ ] Promote report-driven issues from the section below only after real reports justify them.

## Report-Driven Follow-Ups

Only pick these up when real reports show the need; normal audio, no-audio fallback, freezes, and current Worker rooms are currently behaving well in user testing.

- Include receiver A/V sync/playout fields in receiver feedback summaries so sender reports can diagnose title values like `av -600ms` without separately collecting the receiver log.
- Make sender CLI/report NAT status freshness-aware, so `nat_status=connected` does not stay sticky forever after the last receiver feedback packet.
- Detect likely silent system-audio capture, such as Opus packets staying near 3 bytes/packet, and warn that the sender may be capturing the wrong output device even when transport is healthy.
- Investigate small-drift audio time-stretching only if A/V catch-up drops become audible or too frequent; keep hard drops for large real-time recovery.
- Improve the title text if users confuse diagnostic A/V estimates with true audible drift.
- Make `--stream-encoder auto` smarter by detecting hardware encoder input drops and falling back to software or marking that hardware encoder as unhealthy.
- Measure and report max encoder input queue age in milliseconds.
- Prefer dropping stale queued encoder input before it becomes visible latency.
- Revisit UDP pacing if future reports show backlog with fixed 125% pacing headroom; make headroom adaptive instead of fixed if needed.
- Avoid arbitrary UDP queue media drops inside a GOP unless paired with a keyframe strategy.
- Consider keyframe request/force after sender skips frames.

## Conditional / Deferred

- Revisit synced audio rejoin after automatic video-only fallback only if a real report shows audio appears late and needs to become synced automatically.
- Consider making CLI plaintext mode require `--allow-plaintext` after the encrypted/invite UX is smooth enough to avoid surprising users.
- Add a richer debug overlay if title telemetry becomes too dense.
- Consider optional UPnP/NAT-PMP port mapping only if repeated real reports show direct Worker/STUN rooms failing on otherwise important networks.
