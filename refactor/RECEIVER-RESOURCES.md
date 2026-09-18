# Laptop receiver resource attribution

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
report. No production codec or ownership policy has been changed on the basis
of aggregate handle counts.
