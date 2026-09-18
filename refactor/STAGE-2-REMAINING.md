# Stage 2 remaining work

Stage 2 is **not complete**. The opt-in room experience is integrated and tested,
but full adoption parity and physical media acceptance remain. The authoritative
scope is PLAN.md and DETAIL-CHECKS.md; this file groups the remaining work so that
implementation continues in complete batches, not one checkbox per user turn.

## Remaining delivery groups

| Group | What remains | Completion evidence |
| --- | --- | --- |
| Full adoption parity | Guarded normal Home share/join/quick-join and CLI create/join commands now use v2, with mouse/keyboard/controller integration. Remaining: physical control acceptance (Stage 3), legacy diagnostic/report/direct-invite disposition and final default-entry-point audit (Stage 5). | ADOPTION.md and DESKTOP-INPUT.md record routing and actual command/media/input tests. Default launch and existing UDP/control commands remain supported; they are not silently translated. |
| Video acceptance | Display fallback, pinned-source rebuild, GPU cursor composition and minimized/closed states are implemented. Remaining: supported-desktop DXGI acceptance, rotation support if required, physical source identity/privacy/HDR/driver validation, occlusion and latency measurements. | The interactive desktop is available again for adoption regressions; WGC display rebuild passes, but DXGI still reports unsupported in adoption-capture-display.log. CAPTURE-RECOVERY.md and GPU-RECEIVE.md preserve scope limits. |
| Audio acceptance | Physical mono/stereo/surround format negotiation, microphone quality, device switch/unplug/recovery and measured buffering/latency. Microphone-only processing and explicit multichannel conversion are implemented. | Silent PCM/Opus/UI/CLI checks cover the software paths; physical-device evidence remains required. Native driver hangs remain unpreemptible. See AUDIO-PROCESSING.md. |

These are substantial groups. No reliable percentage or turn
count follows from counting the historical checklist entries.

## Dependencies that must not be hidden

- Gaming input transport, backend grants/watchdogs and recording-sink real-channel
  tests, Windows devices, UI/CLI consent and exact-frame coordinate mapping are
  implemented. Physical input acceptance remains **Stage 3**; see [DESKTOP-INPUT.md](DESKTOP-INPUT.md).
  Normal adoption cannot silently remove the existing control/gamepad features.
- Stress, resource-leak, impairment, service-cost, real TLS/NAT and external latency
  acceptance remain **Stage 4**. Those gates have not been passed by localhost tests.
  The capture-only resource matrix and production-owner lifecycle checks are now
  implemented; see RESOURCE-STRESS.md. Full-room restart/continuous runners now
  exist too (ROOM-STRESS.md), but longer capture testing fails the handle bound
  and full memory/two-hour soak acceptance remains open.
  Reproduced retained handles are now traced to WGC/RPC ALPC ports; see
  CAPTURE-HANDLES.md. Later OS cleanup is observed separately and does not waive
  the immediate restart bound.
  Full-room ownership/heap accounting and bounded slow-presentation scenarios now
  exist too (ROOM-STRESS.md). They do not complete the two-hour soak, network
  impairment or physical latency gates.
  The reproduced growing receiver buffer is corrected at the capture audio output
  cadence, with normal A/V sync preserved; see RECEIVER-PACING.md. Continue sustained
  soak/network testing rather than adding another decoder or presentation queue.
- Default enablement and obsolete-code removal remain gated **Stage 5** work.
  Stage 2 integration readiness and the final production cutover are distinct.
- The visual/usability redesign requested by the user remains **Stage 6**, after
  the current refactor. Necessary integration/error/retry UI is allowed now.

## Already integrated; do not rebuild

The Windows controller/UI/CLI group is now integrated with explicit consent,
selected-device polling, per-peer release, panic/focus/source/unplug handling and
recording-device tests. See [CONTROLLERS.md](CONTROLLERS.md). Mouse/keyboard mapping,
confinement and frontend integration are also implemented; see DESKTOP-INPUT.md.
Physical input acceptance remains separate and open. Next is Stage 4 stress/resource
and impairment work, preserving the remaining Stage 2 physical/parity gates.

The guarded normal home workflow and no-JSON CLI create/join commands are
implemented. Home/form navigation shares one pushed directory connection and
sessions use the existing media/runtime pages and asynchronous shutdown. See
[ADOPTION.md](ADOPTION.md). Windows input/consent/controller integration now uses
the shared input port. Keep Stage 2 physical acceptance and the final compatibility
audit open while progressing Stage 4 stress/resource work.

Display-only WGC fallback, pinned output/item recovery, bounded GPU cursor
composition, explicit minimized/closed states and backend diagnostics are
implemented. See [CAPTURE-RECOVERY.md](CAPTURE-RECOVERY.md); keep physical acceptance open.

The receive pipeline now keeps D3D11-decoded NV12 textures through Qt/CLI display,
with bounded ownership, explicit cached readback, hardware telemetry and software
fallback. Decoder recovery is keyframe-gated with a lifetime retry budget, and
live resizing validates delayed output against that frame's own dimensions.
See [GPU-RECEIVE.md](GPU-RECEIVE.md). The remaining video row concerns capture
completion and acceptance; do not implement another decoder or presentation path.

The audio implementation now isolates WebRTC speech processing per microphone
endpoint, bypasses system/process/None, and explicitly converts native multichannel
PCM to stereo. Processing state reaches UI/CLI; silent tests cover mic failure/
retry, switch-away, filter isolation and channel routing. The audio acceptance row
above stays open for real devices. See [AUDIO-PROCESSING.md](AUDIO-PROCESSING.md).

The diagnostics/integration group is implemented: remote presentation/drop/queue
and decoder buffering reports, allowlisted codecs and shared hardware fallback,
capture/recovery states, per-viewer applied preferences and rejection reasons,
partial application, and actual UI preset/manual-setting preservation. See
[DIAGNOSTICS.md](DIAGNOSTICS.md) for contracts, tests and unavailable measurements.
Full legacy feature parity remains in adoption; input and physical latency
evidence remain in Stages 3/4. This does not complete Stage 2 as a whole.

Presentation skips GPU work for hidden/minimized targets, including minimized
application roots above native child video windows, and resumes on fresh frames.
The Qt test fixture's hidden-startup problem is corrected. Earlier visible-but-
occluded reports remain a separate acceptance issue, not a proven fixed defect.
GPU final release no longer invokes the worker from a restricted signaling thread;
both full desktop-inclusive matrices and restricted-thread GPU regression pass.

Both opt-in UI entry points now use the normal AppShell via RoomApplication, with
one-window browser/session navigation, screen-awake state and asynchronous shell
shutdown. The normal home now routes room actions through v2 when explicitly
selected. Remote control and legacy CLI/report compatibility still require the
remaining parity audit; embedding pages and adding commands do not establish it.

Shared room media/runtime, opt-in UI/CLI, pushed directory, validated nickname and
profile defaults, room links/policy/capacity, Auto/Manual settings, per-viewer upload
allocation, live capture/audio/playback selection, no-shared-audio mode, local and
peer diagnostics, retained NV12 presentation handoff, shared renderer recovery,
bounded GPU source scaling, and explicit audio endpoint failure/retry behavior.

Use GPU-SCALING.md, AUDIO-RECOVERY.md, ROOM-UI.md, ROOM-CLI.md and
HEADLESS-TESTING.md for the existing implementation contracts. Build input/control
and remaining parity on these shared AppShell/CLI paths; do not create another
alternative frontend or reimplement these foundations.
