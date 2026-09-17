# Stage 2 remaining work

Stage 2 is **not complete**. The opt-in room experience is integrated and tested,
but normal application adoption and important media work remain. The authoritative
scope is PLAN.md and DETAIL-CHECKS.md; this file groups the remaining work so that
implementation continues in complete batches, not one checkbox per user turn.

## Remaining delivery groups

| Group | What remains | Completion evidence |
| --- | --- | --- |
| Normal UI/CLI adoption | Route ordinary create/join/share/watch and room operations through shared v2 interfaces. Preserve existing functionality, settings and shutdown behavior. Prepare adoption without enabling an unsafe default. | Real normal-entry-point tests, upgrade/configuration behavior, and feature parity. Opt-in RoomSessionWindow/RoomBrowserWindow alone do not complete this. |
| Video pipeline and recovery | Hardware decode and GPU presentation, unresolved preview occlusion, and remaining display fallback/source identity/privacy/cursor/HDR/visible-aperture/resize/device-loss behavior. Preserve fixed settings across fallback. | Silent generated-window/GPU tests, source and decoder failure injection, device-specific acceptance. GPU **source scaling** is done; it is not GPU decoding/presentation. |
| Audio acceptance | Physical mono/stereo/surround format negotiation, microphone quality, device switch/unplug/recovery and measured buffering/latency. Microphone-only processing and explicit multichannel conversion are implemented. | Silent PCM/Opus/UI/CLI checks cover the software paths; physical-device evidence remains required. Native driver hangs remain unpreemptible. See AUDIO-PROCESSING.md. |

These are substantial groups. No reliable percentage or turn
count follows from counting the historical checklist entries.

## Dependencies that must not be hidden

- Gaming input, consent/revoke, watchdogs and coordinate mapping remain **Stage 3**.
  Normal adoption cannot silently remove the existing control/gamepad features.
- Stress, resource-leak, impairment, service-cost, real TLS/NAT and external latency
  acceptance remain **Stage 4**. Those gates have not been passed by localhost tests.
- Default enablement and obsolete-code removal remain gated **Stage 5** work.
  Stage 2 integration readiness and the final production cutover are distinct.
- The visual/usability redesign requested by the user remains **Stage 6**, after
  the current refactor. Necessary integration/error/retry UI is allowed now.

## Already integrated; do not rebuild

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
Normal legacy entry-point parity remains in adoption; input and physical latency
evidence remain in Stages 3/4. This does not complete Stage 2 as a whole.

Presentation skips GPU work for hidden/minimized targets, including minimized
application roots above native child video windows, and resumes on fresh frames.
The Qt test fixture's hidden-startup problem is corrected. Earlier visible-but-
occluded reports remain a separate acceptance issue, not a proven fixed defect.
GPU final release no longer invokes the worker from a restricted signaling thread;
both full desktop-inclusive matrices and restricted-thread GPU regression pass.

Both opt-in UI entry points now use the normal AppShell via RoomApplication, with
one-window browser/session navigation, screen-awake state and asynchronous shell
shutdown. The ordinary legacy home/create/join/control actions and CLI commands
still require adoption; embedding these pages does not establish feature parity.

Shared room media/runtime, opt-in UI/CLI, pushed directory, validated nickname and
profile defaults, room links/policy/capacity, Auto/Manual settings, per-viewer upload
allocation, live capture/audio/playback selection, no-shared-audio mode, local and
peer diagnostics, retained NV12 presentation handoff, shared renderer recovery,
bounded GPU source scaling, and explicit audio endpoint failure/retry behavior.

Use GPU-SCALING.md, AUDIO-RECOVERY.md, ROOM-UI.md, ROOM-CLI.md and
HEADLESS-TESTING.md for the existing implementation contracts. The next adoption
batch should target the ordinary AppShell/CLI paths rather than creating another
alternative frontend or reimplementing these foundations.
