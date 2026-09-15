# Checkpoint A closeout

Closeout date: 2026-09-15. **Gate A passed for native integration/build proof.**
**Full resource acceptance FAILED its closeout recheck and remains open in B/E.**

The user approved a focused closeout against the original PLAN.md Section 5
gate: native codec/audio integration and build reproducibility must work before
wider migration. Full product acceptance is still required in later checkpoints.
No unrun test is marked passed by this closeout.

## Original deliverables and evidence

| Deliverable | Evidence |
|---|---|
| Saved architecture/protocol | PLAN.md, ROOM-PROTOCOL.md and native/Worker shared protocol fixtures |
| Current-backend baseline | Existing 2026-09-14 local legacy run, summarized below; missing measurements explicitly retained |
| Pinned Debug/Release WebRTC artifacts | Content-addressed SDK exports, verified manifests, relocated SDK consumers; BUILD.md and CHECKPOINT-A.md |
| Qt/CLI with matching toolchain | Debug/Release app builds and 10/10 application tests; current Release package rebuilt and launch-checked |
| Local H.264, Opus and encrypted data | Debug/Release media suites 20/20; 26-run headless regression including four-peer sessions |
| Rate/keyframe/reset/frame-ownership proof | MF encoder lifecycle, hardware fallback, owned GPU/CPU frames, capture recovery, settings/adaptation tests |

Latest media evidence: `build/webrtc/settings-{debug,release}-tests.log`,
`settings-headless-release/result.json`, `settings-gpu-final.log` and
`settings-gpu-live.log`. The original crashes, source-close hang and linear COM
event growth have recorded mitigations and passing regression evidence in
CHECKPOINT-A.md. Keep the GraphicsCapture.dll pin, application MTA lease,
owner-thread dispatcher and native-call watchdogs.

## Current resource recheck — failed bound

The current Release binary completed both tests without crash or timeout, with
native exit code zero. The test runner correctly returned failure because the
handle-growth criterion did not pass:

| Run | Completed | Median handles, baseline → final | Bound/result |
|---|---|---|---|
| Full hardware/recovery | 100 cycles, 340.47 s | 386 → 462 (+76) | Maximum +8: FAILED |
| Rapid source close only | 20 cycles, 5.89 s | 329 → 339 (+10) | Maximum +8: FAILED |

Artifacts: `build/webrtc/gate-a-closeout-capture/result.json` and
`gate-a-closeout-rapid/result.json`, including executable SHA-256, exact command,
all samples and logs. Final full-run private memory was 135,069,696 bytes; GDI
objects 10 and USER objects 7. These values do not establish absence of retained
resources or distinguish driver caches from leaks.

The earlier 100-cycle result (380 → 379 handles) remains valid historical
evidence, but does not establish current resource acceptance. The closeout has
not identified the allocating component or fixed this discrepancy. It is a
priority B resource-lifetime investigation and a blocker for clean teardown/
production-cutover acceptance. No application/source fix is claimed in this
closeout. A passes only its original native integration/build gate; the failed
resource checks are neither waived nor marked passed.

## Available legacy baseline

Source: `build/baseline/legacy-loopback-20260914/`, summarized without session IDs
or addresses in `build/webrtc/gate-a-closeout-final/legacy-baseline-summary.json`.
Hardware inventory is recorded in the earlier baseline/reference artifacts in
CHECKPOINT-A.md; it does not prove which enumerated GPU every test selected.

- Debug legacy backend, one local software H.264 sender/decoder, 1920x1080 at
  60 FPS, 12 Mbps setting, Auto bitrate/resolution enabled, 20-second send run.
- Both processes exited successfully. Output averaged 59.9607 FPS; desktop
  changes averaged 4.99672 FPS, so this was mostly repeated desktop content.
- Average capture: 0.200366 ms; average encode: 2.40437 ms.
- Sender CPU time: 12.421875 seconds; receiver CPU time: 8.9375 seconds across
  its 25-second run. These are process CPU time, not GPU utilization or latency.
- Peak sender queue: 189 datagrams / 126 ms; zero reported dropped datagrams.

Limits: local plaintext loopback; no audio or presentation; concurrent dependency
hooks were running. GPU utilization, true display/input latency and legacy
multi-viewer comparisons are unavailable. This is useful baseline evidence,
not a gaming-performance claim or direct comparison with the synthetic v2 scene.
Missing comparative measurements remain in Checkpoint E.

## Distribution audit

Current Release `package-portable` succeeded. The extracted package passed CLI
self-test, UI self-test and GUI startup/shutdown with PATH restricted to Windows
directories and Qt-specific environment overrides removed. Artifacts:
`build/webrtc/gate-a-closeout-package.log` and
`gate-a-closeout-final/package-audit.json` plus three launch logs.

Required executables, qwindows plugin, project/ViGEm notices and WebRTC notices
are staged. The package contains 23 Qt SBOM files. These files do not establish
completion of Qt license-text/source-distribution obligations; those remain on
the release checklist. The installer template recursively consumes the portable
stage, so those same files are included by its staging rule.

No actual Inno compiler is configured or present in the checked installation
locations. The old `build/installer-check` cache points its compiler at cmd.exe
and is only configuration evidence, not a compiled installer. Actual Inno
compilation, fresh-machine install/uninstall and controller-driver provisioning
remain release validation. No installer was executed and nothing was published.

## Scope reconciliation

| Previously accumulated under A | Owning checkpoint; still open |
|---|---|
| Full session coordinator and callback/resource teardown | B, with full 100-cycle/soak acceptance in E |
| Full device recovery integration | B |
| Forced HWND reuse and actual driver removal | E hardware acceptance; injected recovery remains separately passed |
| External gaming latency, GPU utilization, comparative multi-viewer baseline | E; original latency targets unchanged |
| Remaining Qt/distribution obligations and fresh-machine installer | Cutover/release preparation |
| UI settings, scripted gaming input and full headless application scenarios | B/D/E as specified; existing headless components are partial delivery |

Next implementation is Checkpoint B: integrate the reusable media components
behind the serialized session coordinator and shared UI/CLI facade. Advancing
past A is not production cutover approval. Keep normal application media on the
legacy route until the replacement's integration and regression checks support
switching it.
