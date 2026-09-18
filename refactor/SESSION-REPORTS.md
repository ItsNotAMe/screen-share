# V2 session reports and input diagnostics

The v2 session page now has **Save diagnostic report**. It saves a JSON snapshot
under the existing `ScreenShare/reports` application-data directory and displays
the resulting path. Configured absolute report paths remain absolute; relative
UI paths use the existing traversal-safe report resolver. Each default filename
is unique. This is the v2 replacement for troubleshooting snapshots; it does not
claim compatibility with the legacy ZIP report format.

CLI create/join accepts `--report PATH`. It writes the last active observation at
shutdown, or the available failure observation if admission never succeeded.
Relative CLI paths resolve from the working directory. Save failure is reported
as `diagnostic-report` with `saved:false` and a failing process result. Reports are
optional and never uploaded. Atomic writes preserve the previous destination on
failure; output is capped at 1 MiB.

The shared typed exporter includes requested/applied media settings, optional
sender/receiver measurements, pipeline/audio health and input observations. It
excludes admission options, room IDs/names, nicknames, real peer IDs, credentials,
SDP, ICE addresses, source handles and device IDs. Per-report peer aliases correlate
media/input rows without exposing real peer identifiers. No raw logs are bundled.
Stale rates remain unknown, and no external latency result is inferred.

Input diagnostics expose applied/rejected/coalesced counters, queued transitions
and replaceable states, reliable transport blocking, and typed stopping reasons.
Host-local queue wait and backend apply duration use a monotonic clock; they are
not network latency or input-to-photon measurements. They expire after one second
without applied input and clear on permission/connection changes. Heartbeats do
not refresh them. Counters reset when the connection binding changes.

The UI hides technical counters behind **Show input diagnostics**. Congestion,
watchdog expiry and source changes have explicit user-facing explanations. Failure
for the selected peer is no longer hidden by another peer's active permission.
CLI status includes the same counters and stable reason names, retaining numeric
reason values for existing consumers.

Validation: `stage23-final-{release,debug}/result.json` under `build/webrtc` passed
12/12 headless cases each. These include actual UI/CLI report writes, hostile
identifier redaction, unknown/stale metrics, write failures, permission expiry,
bounded state pressure, controller/desktop consent, real media and shutdown.
Tests use silent audio and recording sinks; physical acceptance remains open.
