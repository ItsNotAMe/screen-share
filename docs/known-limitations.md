# Known limitations and release preparation

The measured modular backend was accepted for personal use on 2026-09-19.
The items below remain unresolved or unverified; removing historical reports
neither fixes them nor reopens them as prerequisites for ordinary frontend work.
Active follow-ups live in [the maintenance backlog](../agents/todo.md).

## Current limitations

- Real multi-controller virtual-driver allocation failed on the test desktop:
  a new target reported an occupied XInput index. The sink rejects collisions.
  Single-controller GameSir Bluetooth delivery and repeated physical-reader grants
  passed; this does not prove three simultaneous virtual pads or every device.
- Three test-created virtual instances remained after failed driver tests.
  Administrator cleanup is still outstanding. Verify they are still the owned
  virtual instances before removing exactly `USB\VID_045E&PID_028E\01`, `02`, `03`;
  do not remove the bus or physical controllers. Recheck enumeration afterward.
- The tested laptop's hardware decode Section-handle growth remains unqualified.
  Use the verified software compatibility decoder there. Its hardware encoder
  missed an output deadline and fell back to software.
- Immediate capture-handle bounds, deferred WGC/RPC cleanup and sustained resource
  qualification remain open. Four-viewer software CPU/memory optimization is
  deferred. The reference total host-plus-four-viewer private memory was about
  522 MiB hardware / 924 MiB software versus legacy 398 / 500 MiB; not total GPU RAM.
- Physical input confinement, HDR/multiple adapters, unplug/driver-loss, listening
  quality and external capture/input/A-V latency need broader qualification.
- Internet/NAT/interface changes beyond measured LAN and simulated impairment
  are unverified. Simulated-network wins do not establish real-WAN reliability.
- Provider billing/hibernation reconciliation and 50% free-tier headroom remain
  open; measured request headroom was below that target.

## Distribution

The default client uses the isolated v2 service with a ten-room cap. Installing
it does not migrate or deploy the v1 service. Both endpoints need the v2 client;
v1 rooms and UDP invites are incompatible. See [usage](usage.md).

Before external release: confirm service/channel policy; rebuild and qualify the
portable package and installer on a fresh machine; check upgrades from 0.3.4;
finish distribution notices/source obligations; publish only signed update
manifests. The current updater does not defer installation until a room ends.
Its redesign and active-session deferral remain outstanding.

Historical detailed evidence is retained at Git commit `376d59a`. For example:
`git show 376d59a:refactor/CONTROLLERS.md` recovers the driver investigation.
A failed historical test must not be reported as passing based on newer injected
fixtures. See [performance](performance.md) for the concise measured comparison.
