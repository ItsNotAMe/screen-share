# Stage 2–4 physical acceptance handoff

The user has a Windows laptop and a GameSir Nova Lite and requested concise tests.
The first pass is the five-minute sequence in
[tools/field-test/README.txt](../tools/field-test/README.txt). It covers two-machine
viewing, selected-window privacy/recovery, controller grant/revoke/unplug and
redacted reports. Xbox and PlayStation remain supported software paths, but this
one controller cannot establish their physical compatibility.

## Reproducible private package

Configure Release against a verified exported WebRTC SDK containing its generated
notices (not the raw source build). Enable `SCREENSHARE_BUILD_FIELD_TEST=ON`,
then build the `package-portable` target. `ScreenShareFieldScene` is an optional Qt target
linked to the existing read-only XInput reader. It displays a moving bar, monotonic
host clock, local window key/mouse state and all four host XInput slots. It does
not inject input, open audio/network, install drivers, save raw input or change
firewall rules. Its `--self-test` sends Qt events to its own hidden widget, disables
physical controller polling, and verifies marker rendering and focus cleanup.

```powershell
python scripts/package-field-test.py build/sdk-app-release/ScreenShare-release-windows-x64.zip build/field-test/ScreenShare-v2-field-test.zip
```

The wrapper preserves portable runtime/notices, adds `Start-V2.cmd`, `FIELD-TEST.txt`
and a manifest with source/working-tree identity, input ZIP hash and executable/
launcher hashes. It rejects duplicate/unsafe paths and an existing output. No
release is published. Keep the manifest with both machines' reports.

The launcher explicitly selects v2 and the isolated `screenshare-signaling-v2`
service. It does not change default backend selection. Both known service URLs
returned 404 for `/v2/health` on 2026-09-18; an isolated deployment is therefore
required before a room can be tested. The prepared dry run is:

```powershell
node node_modules/wrangler/bin/wrangler.js deploy --dry-run --config wrangler.v2.toml --var V2_MAX_ROOMS:10 --outdir ../build/webrtc/v2-staging-dry-run
```

Run from `signaling-worker`. This packages the existing v2 Worker, three separate
Durable Object bindings and a ten-room cap. It does not modify the v1 config or
deploy anything. Deployment approval was requested separately; do not infer it
from the user's offer to operate the laptop.

## What the first pass does not establish

External latency requires a known-rate recording and enough independent samples;
the clock, input marker and report counters are aids, not automatic p95 results.
The full HDR/adapter/audio-format/device-loss matrix, additional controllers,
Internet/NAT/interface recovery, immediate WGC handle gate, sustained queue-age/
memory acceptance and production hibernation/billing remain separately tracked.
Do not turn a missing driver, unavailable capture backend or a declined physical
check into a passing result.

## Local preparation results — 2026-09-18

- Final Release and Debug headless matrices: **12/12 each**, artifacts
  `return-packaged-release` and `return-final-restored-debug` under `build/webrtc`.
- Earlier active-desktop Release matrix: **16/16**, `return-desktop-release`.
  The final desktop attempt is separately recorded as blocked by Screen-saver in
  `return-final-release`; no desktop switch or unlock was attempted.
- Software encoder resume/rate tests and sender stats tests pass Release/Debug.
  Hardware encoder/GPU lifecycle and generated-window minimize/restore/privacy
  checks pass. WGC display rebuild passes; DXGI reports unsupported on this HDR
  desktop. Artifacts: `return-native-checks`.
- Field-scene rendering/key/focus self-test passes from the build and an extracted
  notice-complete portable ZIP. The wrapper's ZIP integrity check also passes.
- The diagnostic baseline collapse repetition passes functional thresholds but
  retains 192 ms recent recovery buffering. A motion-hint experiment fails and is
  reverted. Compact hashes/results: [recovery investigation](evidence/recovery-investigation-2026-09-18.json).
  These are not an external-latency pass or proof of stable congestion recovery.

No physical GameSir/laptop result, staging deployment, driver installation,
production change or final Stage 2–4 acceptance is claimed by these local checks.
