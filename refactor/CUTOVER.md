# Modular backend cutover

The user accepted the measured backend on 2026-09-19 and deferred remaining
qualification to BACKLOG.md. Normal UI and CLI startup now use the modular
backend. This is a local client cutover, not an external release or deployment.

## Behavior

- `ScreenShareUi` opens the existing shell/home with the pushed v2 directory.
- `ScreenShare --create-room` / `--join-room ID_OR_LINK` use the modular session.
  `--backend v2` remains an optional compatibility spelling.
- `--room-v2 CONFIG.json` and the explicit browser shortcut remain automation
  entry points into the same implementation.
- The default HTTPS origin is the already deployed isolated v2 Worker,
  `https://screenshare-signaling-v2.bit-yeet.workers.dev`, with its ten-room cap.
  Override it at build time with `SCREENSHARE_DEFAULT_ROOM_ORIGIN` or at startup
  with `--signal-server`. No server configuration or quota was changed.
- Old UDP commands, NAT invites and v1 links fail validation before admission.
  Both endpoints must update and create a fresh v2 room. No v1 credentials or
  active rooms are migrated; the old service remains unchanged.

## Removal and preservation

The old CLI dispatcher, UI session adapter, share/watch/create/join windows,
legacy room-access check and home HTTP fallback are removed. The home now uses
only its shared directory subscription. The old transport and runner remain
in `ScreenShareLegacyTransport` and `ScreenShareAPI`, excluded from the default
production build and linked only by explicit diagnostics/regression targets.
They preserve the reproducible fair baseline; neither shipping executable links
them. Platform capture/codec, input, presentation and updater code remains shared.

Applications now require the pinned native WebRTC artifact/SDK. Old MinGW-only
app configuration fails clearly; debug/release aliases select the native toolchain.
The normal UI updater and signed installer selection remain unchanged. Publishing
a default v2 release or replacing the v1 service is a separate release action.

## Validation

Run Release/Debug default startup and room CLI/UI tests, then build a portable
package and smoke-test its relocated executables. Tests use generated media and
recording input sinks; do not rerun the deferred real-driver allocation probe.
Release and Debug each pass all nine selected checks: default help, retired
commands, executable entry validation (including old links), relocated CLI Qt/TLS
deployment, real-channel synthetic CLI media, normal home/create/join UI flows,
updater package selection, report paths and UI resources. The portable ZIP is
28,935,859 bytes; extracted UI self-test, GUI smoke, CLI help and retired-command
checks pass. Both shipping link graphs exclude legacy transport/API libraries.

Raw logs: `build/cutover-{release,debug}-final-tests.log` and
`build/cutover-package-result.json`. The tested package is
`build/sdk-app-release/ScreenShare-release-windows-x64.zip`.
No external deployment/publication or real-controller allocation test was run.

Both complete application build graphs also pass, including retained comparison
and protocol diagnostics after the library separation. The archive has a
standalone quick-start README rather than repository-only documentation links.
Compact provenance: `evidence/cutover-2026-09-19.json`.
