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
| Audio completion | Microphone-only processing, consistent multichannel handling, and physical device switch/unplug/recovery acceptance. | PCM/Opus contract tests plus separate physical-device evidence. Detected startup/live endpoint failures now keep video running and support explicit same-device retry; native driver hangs remain unpreemptible. |
| Remaining diagnostics and integration checks | Remote presentation/drop/buffering and codec/fallback observations; remaining capture/recovery reasons and settings failure/partial-application behavior. Verify preset/manual-setting preservation through ordinary UI flows. | Shared API, actual UI/CLI and encrypted telemetry tests. Keep unknown values honest; do not equate decoded/source-observed frames with displayed frames. |

These are substantial groups, not four small edits. No reliable percentage or turn
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

Shared room media/runtime, opt-in UI/CLI, pushed directory, validated nickname and
profile defaults, room links/policy/capacity, Auto/Manual settings, per-viewer upload
allocation, live capture/audio/playback selection, no-shared-audio mode, local and
peer diagnostics, retained NV12 presentation handoff, shared renderer recovery,
bounded GPU source scaling, and explicit audio endpoint failure/retry behavior.

Use GPU-SCALING.md, AUDIO-RECOVERY.md, ROOM-UI.md, ROOM-CLI.md and
HEADLESS-TESTING.md for the existing implementation contracts. The next adoption
batch should target the ordinary AppShell/CLI paths rather than creating another
alternative frontend or reimplementing these foundations.
