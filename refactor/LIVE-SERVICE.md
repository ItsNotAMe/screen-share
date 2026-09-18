# Live service lifecycle acceptance — 2026-09-18

The four-viewer public-session proof now runs against the approved isolated v2
HTTPS/WSS service with the production TLS policy enabled. Both the desktop and
laptop pass the Release scenario using the same SHA256-verified executable.
The local HTTP Worker fixture remains supported, with its plaintext exception
explicitly reported. The negative TLS-policy check always uses an explicit HTTP
origin, so supplying HTTPS no longer invalidates that security assertion.

The standalone proof originally deployed Qt DLLs but omitted TLS plugins. Its first
live run failed before admission. It now uses the app's headless Qt deployment
helper and checks TLS availability before starting a room. Deadline errors identify
the scenario call site rather than an indistinguishable shared wait helper line.

## Completed scenario

Each run creates a host and four viewers through real HTTPS admission and WSS
signaling, using the production room/media runtime with synthetic endpoints:

- Decode H.264 and Opus for every viewer; audio is counted and discarded silently.
- Grant/revoke simulated input, observe its synthetic image response and release
  held state through the watchdog. No Windows input is injected.
- Fail and recover capture/playback audio while video and other peers continue.
- Switch audio off/on; apply fixed/manual stream preferences and restore Auto.
- Observe fresh/stale receiver telemetry, restart a connection, leave and rejoin,
  and isolate a failed frame consumer while healthy peers progress.
- Cancel admission, coalesce Stop, wait for runtime drain, reject plaintext in
  production mode, and handle injected capture startup failure.

Release desktop: **pass**, 1,041 decoded frames, 20.972 seconds.
Release laptop: **pass**, 1,074 decoded frames, 20.568 seconds.
Debug desktop: **pass**, 1,027 decoded frames, 17.947 seconds.
Binary SHA256: `ff7bb61e9a727c78bc061c3cbf92c8541ad815aa3efc735d98a47e94f1584771`.
Raw logs/results are in `build/webrtc/live-service-desktop-2` and
`build/webrtc/live-service-laptop-1`. The earlier missing-TLS failure is preserved
in `build/webrtc/live-service-desktop-1`.
The compact, log-hash-verified record is
[evidence/live-service-2026-09-18.json](evidence/live-service-2026-09-18.json).
Debug raw evidence is in `build/webrtc/live-service-desktop-debug-1`.
The final Release local Worker regression also passes; the live runner rejects
HTTP before launching a process or creating an evidence directory.

## Repeating the check

Build `PublicRoomSessionProof` in the SDK proof build. On a second machine, place
the executable beside the matching packaged Qt networking runtime and copy the
runner. Run from PowerShell, using a new evidence directory each time:

```powershell
./scripts/test-room-live-service.ps1 `
  -Executable ./build/sdk-proof-release/PublicRoomSessionProof.exe `
  -Origin https://screenshare-signaling-v2.bit-yeet.workers.dev `
  -OutputDirectory ./build/webrtc/new-live-service-check
```

The runner requires a bare HTTPS origin, enforces a 120-second native-process
deadline, drains both log streams, and records explicit exit codes, binary/runner
hashes and log hashes. It requires every completion assertion and production TLS
mode; missing metrics and nonzero exits fail. Existing evidence is never replaced.
The proof runs in one process and creates no external child processes.

## Boundaries

Media peers reside on the same machine within each automated run; signaling goes
to the actual deployed service. This closes the live TLS/WSS native lifecycle
scenario on both PCs, not cross-machine media/NAT/interface-change acceptance.
The separate user-confirmed LAN viewing/resize and fixed presentation telemetry
are recorded in [PRESENTATION-SIZING.md](PRESENTATION-SIZING.md).

The internal input-marker times (57 ms desktop, 90 ms laptop) are single synthetic
observations, not physical input-to-image latency percentiles. Physical devices,
external latency, congestion/resource gates and billed service usage remain open.
Earlier `RoomCliTests` live-service failures remain unresolved and preserved in
FIELD-TESTING.md; this event-driven fixture does not turn those failures into passes.
