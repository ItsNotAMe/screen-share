# Laptop receiver resource attribution

## Native presentation and recovery — 2026-09-19

The final five-minute desktop hardware-host → laptop software-viewer run now
**passes native GPU presentation**, using the production `LatestRoomVideoFrame`
handoff and `FramePresentationSession` backend. It delivers **47.7 rendered FPS**
and **48.3 fresh decoded FPS**, with zero invalid markers, zero rendering errors,
no occlusion and a hardware graphics device with maximum frame latency one.
The stream is 1920×1080; the initial test-window client is 944×501, not a
full-screen or complete Qt UI measurement. Automatic resizing to two sizes,
bounded dropping while minimized, and fresh rendering after restore all pass.

The viewer uses 68.7% of one CPU core. First/last 30-second median private memory
is **89.1 / 90.5 MiB**, peak **94.8 MiB**; handle medians fall **659→645**.
Section counts are **11→11→9** (warm-up, after streaming/recovery, after stop).
The previously identified Intel graphics DLL reports version **32.0.101.8517**.
This qualifies the finite native presentation run in compatibility mode, not
hardware decoding, indefinite resource bounds or external gaming latency.

The runner uses an existing signed-in desktop through a temporary non-admin
task, removed afterward. A stable, hash-verified runtime path avoids creating a
new firewall application path on every run; evidence/ZIPs stay separate. No
firewall rule, driver, password, physical input or audible output is changed.
The test window temporarily requests display wakefulness and releases it on exit.

Earlier failures are retained in the [compact evidence](evidence/presentation-compatibility-2026-09-19.json):
two invisible SSH-window runs, an occluded interactive run, an interrupted
four-minute run without native samples, and a recovery smoke affected by a
network prompt that the user confirmed. Two later attempts failed before media
in runtime-staging preflight. The corrected stable-path five-minute run had no
occlusion and stable resources but only **44.4 rendered FPS**, failing the same
45-FPS gate. Replacing the proof's coarse polling sleep with frame-ready/message
wakeups produced the final passing run without lowering that gate. The production
Qt renderer already wakes on work; this is a fixture scheduling correction,
not a new legacy/v2 performance win. Connection-state failures now retain samples
and room phase/error instead of throwing away the measurements.

Raw final evidence: `build/presentation-laptop-event-driven`. The retained
polling control is `build/presentation-laptop-final3`. Release/Debug proof builds
and UI/CLI tests pass. PowerShell 5.1 and 7 reject all 51 malformed timing/load/
decoder/presentation cases, including missing recovery and false GPU claims.
Repeat instructions: [HEADLESS-TESTING.md](HEADLESS-TESTING.md).

## Decoder compatibility path

The receiver now supports a local, explicit software-decoder choice without
changing the host encoder, capture or transport. The existing join form saves
Automatic / Software (compatibility); CLI viewers use `--decoder software`,
and room JSON uses `"decoder": "software"`. Automatic remains the default.
Wrong-role, malformed and unknown values are rejected before joining. CLI
overrides do not rewrite the saved preference. No codec choice is sent as a
host room mutation or inherited from a room link.

Release/Debug builds and UI/CLI integration checks pass, including propagation
to the runtime, saved settings, corrupt preferences and failed writes. Evidence
validation explicitly distinguishes hardware encoding plus software decoding
from hardware on both endpoints, checks every active host sample, and rejects
mislabelled mode values. The same fresh-image requirements apply to both modes.

The final local real-runtime smoke passes at 47.9 fresh 1080p FPS with zero
invalid images and about 54% of one CPU core on the receiver. This is a local
CPU pixel-consumer result, not a laptop resource or physical presentation pass.
The first laptop attempt timed out during SSH setup before media started;
the user woke the laptop, and the subsequent 20-second laptop smoke passed at
47.6 fresh FPS, no invalid images, and 1.39 CPU cores. Its Section count stayed
5→5.

The five-minute laptop run **passes the same media checks**, with hardware
encoding observed on the host and software decoding in every active receiver
telemetry sample. It delivers **45.4 fresh FPS**, zero invalid images, and uses
about **1.35 CPU cores**. Viewer Section handles stay **5→5→5** (warm-up, end,
after stop). First/last 30-second private-memory medians are **31.3 / 32.7 MiB**,
peak 38.7 MiB; handle medians fall **601 / 582**. This avoids the earlier
five-minute hardware receiver's 118→157 MiB and 645→833-handle trend in the
measured CPU-consumer path. It does not prove an indefinite bound or qualify
hardware decoding, GPU presentation, 60 delivered FPS or external gaming latency.

Raw runs are `build/receiver-software-laptop-smoke` and
`build/receiver-software-laptop-sustained`; the final local control is
`build/receiver-software-local-final`. The
[compact evidence](evidence/decoder-compatibility-2026-09-19.json) retains their
binary/source/runner/report hashes, resource windows and the kernel trace.
The initial SSH failure is preserved separately. Final evidence tests pass in
PowerShell 5.1 and 7: the previous 18 malformed load cases plus five decoder-mode
cases are rejected, and both explicit modes are accepted only when matched.

The software path avoids the hardware decoder's per-frame owned NV12 texture
allocation. The existing UI's CPU-frame presenter reuses its luma/chroma
textures, but these CPU-consumer media tests do not validate sustained physical
GPU presentation. Software decoding is a compatibility choice with a CPU
tradeoff, not a claim that hardware decoding is universally slower or broken.

## Kernel allocation trace

The bounded debugger run in `build/receiver-section-trace/native-v3.log` captures
the direct-creation interval. Seven retained unnamed Section handles have
opening process ID **4 (System)** and zero granted access in the test process.
The other four retained handles are application events/I/O completions. This
supports a graphics/kernel retention issue, not a missing application
`CloseHandle` call. Allocation call stacks are unavailable; the trace does not
identify an exact driver function or establish that Intel alone is responsible.
The first two debugger attempts did not collect a valid interval and remain
preserved. Debugger timings are not performance evidence. No driver, system
service, account or machine policy was changed.

## Earlier hardware diagnostics

The five-minute hardware LAN delivery result remains valid, but it does not
close sustained resource acceptance. This investigation separates the increasing
Windows handle count from the deferred goal of using less memory than legacy.

The load proof now records handle-type counts after warm-up, after streaming,
and after session shutdown. Windows Process Snapshotting owns and releases each
snapshot and walk marker; object names and addresses are not retained.

The 90-second laptop receiver diagnostic increased Section handles from 12 to
77; all 77 remained after session shutdown. This run delivered 49.6 fresh FPS
but had five unreadable scene markers, so its media result is **failed**, not an
additional acceptance pass. Its raw report is under
`build/receiver-handle-types`; do not suppress those invalid frames.

The graphics-only probe removes Media Foundation, room signaling, transport,
capture, audio and presentation. Its initial three 30-second phases measured:

| Phase | Section handles before / after |
| --- | --- |
| Idle device | 3 / 3 |
| Upload and release NV12 textures | 3 / 25 |
| Upload, read pixels and release | 25 / 50 |

The readback scratch allocation count was exactly one. All 50 Section handles
remained after releasing the device. This reproduces growth without a decoder
or network, but alone does not prove which graphics component owns it.

A second run adds direct `ID3D11Device::CreateTexture2D` calls, releasing each
texture before the next iteration. Its 20-second phases show Section counts
3→3 idle, 3→17 direct creation, 17→32 owned-frame upload, and 32→49 with
readback. The direct phase bypasses the frame wrapper and its cross-thread
dispatch. Growth therefore reproduces below the media/room implementation;
this is evidence against blaming the v2 wrapper, not proof of a particular
driver defect or a safe long-term bound. Raw evidence:
`build/receiver-gpu-raw-resources.json`.

The final identical-build control uses four ten-second phases. The laptop's
selected adapter is **Intel Arc Pro Graphics**: Section counts 3→3→10→17→26.
The desktop's **NVIDIA GeForce RTX 5070 Ti** reports 4→4→6→9→6. Both reuse one
readback scratch texture. These short controls demonstrate a machine-specific
difference; they do not establish whether it is a driver, OS integration or
another graphics component, or prove an unbounded leak from finite samples.
The [compact evidence](evidence/receiver-resources-2026-09-19.json) retains raw
output hashes, final binary/source hashes, phase resources and the failed LAN
diagnostic. Earlier probe variants are explicitly distinguished from the final
build. Release and Debug builds and both room evidence CTests pass.

**Attribution is complete enough to avoid a speculative media rewrite; the
underlying growth remains unresolved.** Next investigate the laptop graphics
environment using this minimal reproducer, then repeat the full hardware LAN
and sustained-resource checks after a demonstrated fix or bounded behavior.
No laptop driver, account, firewall or machine policy was changed. Stages 2–4
also retain the physical device/input/audio, external latency, Internet/NAT and
capture-restart acceptance items in STAGE-2-4-ACCEPTANCE.md.

## Reproduction

### Longer unattended laptop probe (2026-09-19)

Four 120-second phases confirm continued Section-handle growth: idle 4, direct
upload 93, owned upload 183, upload/readback 282. After device release, 282
Sections remain. Total handles rise from 279 to 560, then fall to 527; private
memory ends near 103 MiB and is not monotonically increasing. This is an
eight-minute standalone D3D probe, not a streaming acceptance pass. The hardware
path's long-term resource bound remains unqualified; the measured software
decoder compatibility path remains the available workaround.

Raw evidence: `build/unattended-closeout-20260919/gpu-bound-final-20260919`.
No driver, registry, power or machine-policy change was made.

`CrossMachineRoomProof.exe gpu-resources 30` runs four phases of 30 seconds:
idle, direct D3D texture creation/release, the owned-frame upload path, then
upload with checked CPU pixel readback. It emits JSON with resource and handle
counts per phase and after device release. It needs no room, administrator
account, speakers, keyboard/mouse input, or real desktop capture. Duration is
bounded to 10–120 seconds per phase. `diagnosticOnly: true` deliberately does
not claim streaming, physical latency, or resource acceptance.

For streaming reproduction, use the hash-verified laptop command in
[HARDWARE-LAN.md](HARDWARE-LAN.md). Its runner preserves both stdout and stderr
on remote failure; PowerShell module startup messages no longer hide the native
report. The compatibility choice is explicit; there is no speculative texture
ownership change or automatic vendor-wide hardware blacklist.
