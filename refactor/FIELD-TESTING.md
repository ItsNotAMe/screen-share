# Stage 2–4 physical acceptance handoff

The user has a Windows laptop and a GameSir Nova Lite and requested concise tests.
The first pass is the five-minute sequence in
[tools/field-test/README.txt](../tools/field-test/README.txt). It covers two-machine
viewing, selected-window privacy/recovery, controller grant/revoke/unplug and
redacted reports. Xbox and PlayStation remain supported software paths, but this
one controller cannot establish their physical compatibility.

## Reproducible private package

Current laptop package (2026-09-19): the **ScreenShare v2 Compatibility Test**
desktop shortcut points to `ScreenShareTests/decoder-compat-4f0f5fc/ScreenShare-release-windows-x64`
in the test account. It includes the saved **Software (compatibility)** viewer
decoder option; host encoding is unchanged. Source commit `4f0f5fc` was clean.
All manifest hashes were checked after extraction; deployment evidence is
`build/decoder-compat-deployment.json`. Earlier deployment records below remain
historical. The shortcut was created without launching the app.

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
returned 404 for `/v2/health` before deployment on 2026-09-18. The prepared dry run was:

```powershell
node node_modules/wrangler/bin/wrangler.js deploy --dry-run --config wrangler.v2.toml --var V2_MAX_ROOMS:10 --outdir ../build/webrtc/v2-staging-dry-run
```

Run from `signaling-worker`. This packages the existing v2 Worker, three separate
Durable Object bindings and a ten-room cap. It does not modify the v1 config or
deploy anything. The user subsequently explicitly approved deployment, completed
Cloudflare reauthentication, and the isolated service was deployed with the same
ten-room cap. Version: `74a4354d-b242-475e-8eaa-fa18d3bb4b76`. Both PCs received
HTTP 200 with `{"v":2,"status":"ok"}` over HTTPS. The cap is also pinned in
`wrangler.v2.toml` to preserve it on later deployments. V1 was not modified.

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

No physical GameSir result, driver installation or final Stage 2–4 acceptance is
claimed by these local checks.

## Connected laptop preparation — 2026-09-18

- Verified SSH host fingerprint out of band and connected as a dedicated standard
  local test account. Its interactive desktop is active. No password or private key
  is recorded in repository files. Windows build is 26200 / 25H2; CIM inventory
  was denied to the account, so no GPU identity was inferred.
- Transferred the original `29cbd2c` private ZIP, verified SHA256
  `869b57a3e6b830b903a1e02b5d8e8d3d9751aec6e105c3c56acf087b86b44fd1`,
  and extracted under the test account's `ScreenShareTests/build-29cbd2c` folder.
  Field-scene offscreen self-test exits 0. A desktop shortcut launches that build;
  the user confirmed its Home screen opens. This is not video/control acceptance.
- Additional standalone test executables are separate from the packaged manifest.
  Initial PowerShell Start-Process evidence lacked exit codes; it is not accepted.
  A corrected System.Diagnostics.Process runner retains handles, drains both output
  streams, enforces deadlines and records explicit exit codes/hashes.
- Software encoder and decoder tests exit 0. Hardware/GPU encoder test exits 1
  with `Hardware lifecycle test fell back` in the SSH session. Interactive desktop
  hardware capability remains unverified; this is not a hardware pass.
- The CLI test assumed its supplied origin was plaintext. Its security check now
  explicitly rejects a loopback HTTP URL regardless of the service under test.
  Added failure diagnostics expose playback/role admission failures. The original
  local Worker regression passes after these changes. Laptop live-service runs
  still fail: first playback checks, then silent-video/frame-count checks after
  reaching later settings/media assertions. Fixed localhost-oriented timing may
  contribute; the cause is not established and assertions were not weakened.
- Raw laptop results/logs: `build/webrtc/laptop-first-tests/native-checks-2`
  and `native-checks-3`, with the transferred package verification alongside. Deployed
  service logs: `build/webrtc/v2-staging-deploy-authenticated.log`.

Continuation: two-PC video/resize and the corrected laptop presentation counters
are verified (PRESENTATION-SIZING.md). Event-driven live HTTPS/WSS lifecycle now
passes on each PC (LIVE-SERVICE.md). Earlier timed CLI failures are now resolved
as fixture ordering errors, and automated LAN media/recovery passes both directions
(TWO-MACHINE.md). No physical-input or external-latency gate is inferred from this.
The GameSir is Bluetooth-connected to the desktop, so laptop controller discovery
is not yet tested. Privacy, physical controls, Internet/NAT and external latency
still need their own evidence.

## Removing the temporary connection later

Do this after ending test processes and collecting their reports, not during a
test. On the laptop, remove the test desktop shortcut and `ScreenShareTests`
folder after checking their contents. From the normal administrator account,
`Remove-LocalUser -Name ScreenShareTest` removes the temporary login. Its Windows
profile can be removed separately through the User Profiles settings after sign-out.
The accidental public key in the normal account was removed during setup; preserve
any unrelated authorized keys.

If OpenSSH Server was enabled only for this test, stop/disable `sshd`, remove its
`OpenSSH-Server-In-TCP` firewall rule and uninstall the OpenSSH.Server optional
capability. Restore the Wi-Fi Public profile only if that is the desired original
setting. Do not remove the SSH client/authentication agent or unrelated SSH files.
On the main PC, the dedicated `.ssh/screenshare_laptop_ed25519` private/public pair
and `.ssh/screenshare_laptop_known_hosts` are the connection-specific files.
Removing the laptop connection does not remove the independently deployed v2 service.
