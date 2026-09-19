# ScreenShare 1.0.0

ScreenShare 1.0.0 introduces the rebuilt room-based streaming backend and a
redesigned desktop interface for hosting, viewing and managing remote control.

- Live display/window previews, source switching, automatic room settings,
  in-window settings and diagnostics, and shared-screen fullscreen mode.
- Independent mouse, keyboard and controller grants, including direct grants
  from the host, with fixes for intermittent input loss.
- Steady capture cadence on static desktops, improved audio continuity and
  resolution/resize handling, plus title-bar fixes for scaled displays.
- Verified updates wait until the room ends and require **Install and restart**.

## Upgrade from 0.3.4

Run `ScreenShare-Setup-1.0.0-windows-x64.exe`, or extract the complete portable
ZIP to a separate directory. Both host and viewer must upgrade: 0.3.4 rooms
and old direct/UDP invites are incompatible. Create a new room after upgrading.
The release uses the separate v2 service with a ten-room limit; it does not
migrate or shut down the old service.

The owner confirmed two-computer input/audio fixes and an installation over
0.3.4 on the second computer. Automated regression and package checks are
recorded in the repository. This is not fresh-machine or broad hardware certification.

## Downloads and source

Use Setup for installation and controller-driver provisioning. The portable
ZIP does not install the driver. The update manifest verifies download hashes
with the existing release signing key. Application/Setup binaries are not
Authenticode-signed; Windows may show an Unknown Publisher warning.

Matching Qt 6.10.3 source archives are separate release assets; copyright and
license notices are bundled in both packages. See THIRD-PARTY-NOTICES.md.

## Known limitations

Multi-controller allocation remains under investigation. Use software decoding
on laptops affected by hardware-decoder resource growth. Wider WAN, HDR,
multi-adapter and long-duration resource testing remains incomplete. See
docs/known-limitations.md for details.
