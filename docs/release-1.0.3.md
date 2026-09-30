# ScreenShare 1.0.3

- Fixed reconnection getting stuck on Connecting when a replacement room socket
  arrives before the old socket's disconnect notification. Both sides now rebuild
  the affected media peer, restoring remote controls and viewer telemetry.
- Other viewers keep streaming while an individual viewer reconnects.

The reconnection fix is deployed in the room service and also supports existing
1.0.2 clients. If a session is already stuck, leave and rejoin the room.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
