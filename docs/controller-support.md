# Controller support

ScreenShare's controller MVP reads one XInput controller on each granted viewer and creates one
independent Xbox 360-compatible virtual controller for that viewer on the host. The host's physical
controller is not intercepted or hidden. Mouse and keyboard remain exclusive capabilities, while
gamepads may be granted to multiple viewers.

## Installed controller runtime

The supported prerelease is `ScreenShare-Setup-*.exe`. ScreenShare Setup runs elevated and provisions
the hash-pinned, signed controller driver as part of installing or updating ScreenShare. The pinned
user-mode client is built from source and shipped beside the application. Users never download or
install a separate controller program, and launching ScreenShare never installs, updates, or repairs
the driver.

The portable ZIP deliberately does not provision a system driver. It can host controllers only on a
machine where a compatible driver is already present; otherwise gamepad grants are disabled while
screen sharing and mouse/keyboard control continue to work.

ViGEm is retired, so this packaged backend is for the v0.3 prerelease rather than the long-term public
release. The setup validates the exact driver artifact at build time and aborts if install-time
provisioning fails.

## Capacity and safety

The host counts local XInput controllers before creating virtual devices and always reserves at least
one of XInput's four player slots for local play. Therefore no more than three remote virtual pads are
granted, and fewer are allowed when multiple local controllers are already connected.

Each virtual device belongs to a stable viewer identity. Complete state snapshots are encrypted and
replay-protected by the existing control channel. A 300 ms keepalive timeout, revoke, panic hotkey,
viewer disconnect, controller unplug, backend error, or session shutdown submits neutral input and
destroys only the affected virtual device.

## Long-term signed backend

The backend interface allows replacement with a maintained commercial runtime or a ScreenShare-owned,
production-signed driver. A VHF implementation would provide standard HID gamepad behavior, but it
must be validated against XInput-only games before replacing the Xbox-compatible prerelease backend.
Any replacement keeps the same rule: driver lifecycle belongs to ScreenShare Setup and updates, never
normal application startup.
