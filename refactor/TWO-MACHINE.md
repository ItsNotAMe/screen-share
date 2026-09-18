# Two-machine room acceptance — 2026-09-18

The silent native scenario passes in both LAN directions: desktop host → laptop
viewer and laptop host → desktop viewer. Both use the approved HTTPS/WSS service,
the production RoomSession/NativeRoomRuntime, and software H.264/Opus endpoints.
The laptop runs as the existing standard SSH test account. No controller driver,
firewall configuration or account permissions were changed.

## Verified

- Actual media travels between the two PCs; HTTPS admission and WSS signaling use
  production TLS policy, with no plaintext exception.
- Twenty authorized synthetic key press/release cycles produce remote decoded
  image changes. The host's recording sink receives all 40 events; no OS input
  is injected. Audio samples are decoded, counted and discarded without speakers.
- Fixed 320×180/manual bitrate settings take effect, the viewer requests an actual
  connection restart, then leaves and joins with a fresh identity.
- The new viewer receives the retained settings; restoring 640×360/Auto propagates
  to the receiver and fresh host telemetry. Both runtimes release on shutdown.

| Direction | Result | Decoded frames | Internal response p50 / p95 / p99 |
| --- | --- | --- | --- |
| Desktop → laptop | Pass | 388 | 94.090 / 108.425 / 108.685 ms |
| Laptop → desktop | Pass | 391 | 92.913 / 107.431 / 108.416 ms |

Each timing row contains **20 samples**, using the viewer's monotonic clock from
synthetic input submission to decoded response. Percentiles use nearest rank.
Release/clear frames are observed between presses, so an old response cannot
satisfy the next sample. These are not physical controller/display measurements,
1080p60 hardware-load tests, Internet/NAT tests or proof of the Gaming p95 target.
The source is a low-complexity generated 640×360 scene, not gameplay.

Binary SHA256: `9a811f10f836864eef18b27ac67cdff23d09b0a3e8126d02194e383e15657e14`.
Raw accepted evidence: `build/webrtc/cross-desktop-host-4/{host,viewer}` and
`build/webrtc/cross-laptop-host-1/{host,viewer}`. Endpoint placement is established
by the launcher and independently collected host/viewer machine reports.
Hash-verified results, original failures and timing samples are retained in
[evidence/two-machine-2026-09-18.json](evidence/two-machine-2026-09-18.json).

The expanded Release desktop matrix passes **17/17**, including the new delayed
viewer, generated WGC capture, hardware presentation and UI/CLI input integration.
Artifact: `build/webrtc/stage-2-4-connected-desktop-final/result.json`. Its preceding
run reached a Windows CLI result-parsing failure after the native checks passed:
the sizing regression printed a diagnostic line before the JSON. Sizing counts now
belong in the single JSON result. The original parse failure is preserved in
`stage-2-4-connected-desktop`; no media assertion was removed to fix it.

The first native desktop-host run (`cross-desktop-host-3`) completed successfully
but its viewer validator rejected Windows PowerShell's JSON Decimal values.
That failed validation remains preserved. The validator now explicitly accepts
finite nonnegative Decimal/Double/integer numbers and rejects strings, booleans,
nulls, NaN/infinity, negatives and missing/wrong sample counts. Tests pass under
both Windows PowerShell 5.1 and PowerShell 7. Earlier launcher failures occurred
before room creation due to an inherited incompatible module path; the launcher
uses the native Windows PowerShell module directory in its child environment.

## Repeat without mouse/keyboard automation

Build `CrossMachineRoomProof` from `tools/webrtc-proof`. Place it alongside the
matching portable Qt networking runtime on both machines. Copy
`scripts/test-room-live-service.ps1` **and** `scripts/RoomLiveEvidence.ps1` together.
Use unique readiness and output paths for every run. On the host:

```powershell
./test-room-live-service.ps1 -Executable ./CrossMachineRoomProof.exe `
  -Origin https://screenshare-signaling-v2.bit-yeet.workers.dev `
  -Scenario cross-host -ReadyFile ./ready.json -OutputDirectory ./host-result
```

Read the generated `ready.json` (it contains the temporary unlisted room ID).
Within 60 seconds, run on the other PC:

```powershell
./test-room-live-service.ps1 -Executable ./CrossMachineRoomProof.exe `
  -Origin https://screenshare-signaling-v2.bit-yeet.workers.dev `
  -Scenario cross-viewer -RoomId <roomId> -OutputDirectory ./viewer-result
```

Both commands enforce a 120-second process deadline and record executable,
runner, validator and log hashes. Require **both** results to pass and confirm
different endpoint machines. Reverse the roles for the other direction.

## CLI live-service failure resolved

The old CLI test started host changes at fixed offsets before the viewer had
necessarily joined, decoded ten frames and recovered its deliberately failed
playout. The reproduced failure had 16 original frames, 90 changed frames and
289 audio blocks, but never observed initial silent video before the host changed
audio. It was a fixture ordering failure, not evidence that media stopped.

The test now arms the existing parsed schedule on its owner thread after initial
video/silence is observed. Playback retry follows the observed failure. Subsequent
timed changes retain their spacing; all original progress, recovery, input,
privacy/redaction and report assertions remain. A 30-second correctness deadline
bounds failure; successful runs exit on completed assertions (about nine seconds
over HTTPS). Product scheduling and congestion behavior are unchanged.

Live CLI passes on both PCs; Release and Debug local scenarios pass. An additional
three-second delayed-viewer regression also passes and is registered in CTest as
`room-v2-cli-delayed-viewer`. Raw baseline failure is preserved at
`build/webrtc/cli-live-baseline-desktop`; corrected live results are at
`cli-live-ready-desktop` and `cli-live-ready-laptop` under the same root.
Use the live runner with `-Scenario cli -Executable ./RoomCliTests.exe` to repeat.
