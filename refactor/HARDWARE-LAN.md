# Two-machine 1080p hardware load — 2026-09-19

This group extends the prior 640x360 software LAN checks with the production
Windows capture/room/media path at fixed 1920x1080, 60 FPS and a 12 Mbps ceiling.
The desktop captures only an owned generated motion window. The laptop decodes
and reads back image pixels, validates dimensions and complementary frame-ID
markers, and counts distinct images. Audio uses paced silence and discarded
playout. No OS input or physical controller events are sent.

The scene implementation is shared with the legacy comparison, extracted without
changing its implementation. All comparison runners now retain its source hash.
This is actual two-machine media over the approved HTTPS/WSS room service and LAN,
not two local peers using a remote signaling server.

## Repeat as one command

Build `CrossMachineRoomProof` in the Release proof configuration, then run:

```powershell
python scripts/test-room-hardware-load.py build/sdk-proof-release build/hardware-lan-NEW --peer ScreenShareTest@192.168.1.113 --identity "$env:USERPROFILE/.ssh/screenshare_laptop_ed25519" --known-hosts "$env:USERPROFILE/.ssh/screenshare_laptop_known_hosts" --origin https://screenshare-signaling-v2.bit-yeet.workers.dev --seconds 300
```

Use an already-authorized SSH account and pinned host key. Each run uses a fresh
directory under that account's `ScreenShareTests` directory, verifies the ZIP hash
before extraction, and checks identical executable/runner/validator hashes on
both endpoints. It preserves failed evidence and rejects same-machine endpoints.
The launcher uses a process-only RemoteSigned policy for these local scripts;
it does not change persistent account policy, firewall rules or driver installation.

Add `--decoder software` for the viewer compatibility check. Host encoding stays
hardware; the report and strict per-sample decoder assertions explicitly require
software on the receiver. The default remains `--decoder hardware`, with the
original hardware-only requirements. A software-mode pass never closes hardware
decode acceptance. Both modes retain the same fresh-FPS and invalid-image gates.

For sustained native presentation and automatic resize/minimize/restore checks,
add `--consumer presentation --interactive-viewer --runtime-name presentation-runtime`.
The test account must already have an unlocked desktop; the runner uses a
temporary standard-user task, not the invisible SSH window station. Reusing the
runtime path avoids assigning a new firewall application path for every run.
See [HEADLESS-TESTING.md](HEADLESS-TESTING.md) for prerequisites and limits.

The host waits for the viewer, and the viewer warms up before a timed load interval.
Both record one-second private-memory, working-set and handle samples, process
CPU and post-stop resources. The host records actual hardware encode counters,
fallback counts and fresh receiver codec telemetry. Every active host sample must
identify hardware on both ends; a single software fallback fails. Every viewer
sample must make fresh-image progress; average fresh FPS must be at least 45,
with no unreadable markers. Native and outer process deadlines bound failures.

## Measurement boundaries

Frame IDs establish distinct image delivery. The test never subtracts clocks
from different machines and does not claim physical display or input latency.
The receiver is a CPU pixel consumer, not a physical GPU presentation surface.
This one-viewer LAN run does not establish four-viewer Internet/NAT behavior,
audio quality/A/V skew, controller compatibility or the two-hour acceptance gate.
Post-stop resource samples retain Windows runtime allocations; they do not waive
the separate immediate capture-handle bound in CAPTURE-HANDLES.md. Relative memory
optimization remains backlogged; sustained leaks and practical memory remain relevant.

The initial launcher failures are retained in `build/hardware-lan-smoke` and
`build/hardware-lan-smoke-2`: an inherited module environment hid Get-FileHash,
then the laptop's Restricted execution policy prevented its local script from
starting. Neither is a media pass. The corrected 20-second smoke passes with
52.5 fresh FPS and actual hardware codec telemetry on both machines.

## Five-minute result

The final run passes hardware media delivery: **52.8 fresh FPS**, zero invalid
images, no software fallback, and hardware encoder/decoder telemetry throughout
the active interval. The desktop is MYCALCULATOR; the laptop is MYCALC. This is
one viewer, not the four-viewer gate. Raw evidence: `build/hardware-lan-sustained`.
The [compact evidence](evidence/hardware-lan-2026-09-19.json) retains source/binary/
report hashes, resource intervals and both earlier launcher failures.

| Endpoint | CPU (one core = 100%) | Peak private MiB | First / last 30-s median private MiB | First / last 30-s median handles |
| --- | --- | --- | --- | --- |
| Desktop host | 39.2% | 184.8 | 170.2 / 171.5 | 910 / 893 |
| Laptop viewer | 28.7% | 161.8 | 118.1 / 156.6 | 645 / 832.5 |

**Sustained resource acceptance remains open.** The laptop's roughly 38 MiB
private-memory increase and steadily rising handles need attribution and a
long-term bound. This is not the deferred goal of beating legacy's memory number.
At stop its private memory falls to about 122 MiB and handles to 712, but that
does not establish a flat steady-state trend or full cleanup. The desktop stays
roughly flat. No speculative decoder/driver fix was applied from aggregate
counters alone. Next isolate laptop receiver allocations, then finish the
consolidated physical/device/network acceptance pass.
The subsequent [resource attribution](RECEIVER-RESOURCES.md) reproduces Section
handle growth with direct D3D texture creation, outside decoding/networking and
the v2 frame wrapper. It does not yet establish a long-term bound.

Release and Debug proof builds pass; the comparison executable builds after the
unchanged scene extraction. Evidence tests pass in PowerShell 5.1 and 7: valid
host/viewer reports are accepted, 18 malformed/fallback/stalled load reports and
nine malformed timing reports are rejected. Release/Debug evidence CTests and
the comparison evidence CTest pass. No production backend behavior changed in
this measurement group.

## Reverse direction: 2026-09-19

The laptop-host → desktop-viewer five-minute run delivers **49.49 fresh FPS**,
14,848 distinct images and zero invalid images at 1920×1080. Both sessions remain
healthy. This passes the viewer's unchanged 45-FPS delivery gate, but **fails the
hardware-host gate**: laptop encoding falls back once to `mf-h264-software`.
Host-side receiver telemetry confirms the desktop uses `mf-h264-hardware` decoding.
The viewer's own codec-observed fields are not populated; do not infer codec choice
from its requested mode alone.

| Endpoint | CPU (one core = 100%) | Peak private MiB | First / last 30-s median private MiB | First / last 30-s median handles |
| --- | --- | --- | --- | --- |
| Laptop software host | 189.4% | 437.2 | 279.8 / 272.1 | 803 / 794 |
| Desktop hardware viewer | 17.0% | 110.2 | 104.4 / 107.2 | 710 / 698.5 |

Desktop Section handles stay at 13 during streaming and fall to six after stop.
This is useful bounded five-minute fallback evidence, not physical presentation,
gaming latency, four-viewer throughput or a long-term leak certification.

A separate interactive-session `MfHardwareAdapterTest` also fails with
`Hardware frame exceeded 500 ms output deadline`, without capture or networking.
The laptop hardware encoder is therefore not qualified. The deadline has not
been increased to hide the stall. This does not overturn the desktop hardware
result, and both legacy and v2 share the affected codec implementation.

The first reverse attempt failed its fresh-image startup check. The generated
scene now opts its window thread into per-monitor DPI awareness so markers remain
at physical pixel coordinates on scaled displays. This fixture change is shared
with the legacy comparison. The corrected run passes image validation; no
production capture path or acceptance threshold changed. Startup failures now
preserve image counters instead of losing the diagnostic evidence.

Raw runs are in `build/reverse-load-20260919` and
`build/reverse-load-dpi-20260919`; the first host used an older validator and is
retained only as a failed diagnostic. The corrected endpoints use matching runner,
validator and executable hashes. [Compact evidence](evidence/reverse-load-2026-09-19.json)
retains both outcomes, hashes, resource summaries and standalone diagnostic logs.
Release/Debug proof builds, controller parser tests and live-evidence tests pass.
Both temporary remote load tasks were removed after completion.
