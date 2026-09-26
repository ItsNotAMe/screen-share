# ScreenShare 1.0.1

- Expanded diagnostic reports with connection startup and failure timelines,
  ICE/DTLS transport measurements, codec and GPU errors, rendering, bitrate,
  frame delivery, CPU and memory history. Reports retain failed connections and
  correlate host/viewer evidence without exporting network credentials or SDP.
- Added default STUN discovery for internet connections when no ICE servers
  are configured. STUN is not a TURN relay and cannot connect every NAT pair.
- Prevented stalled viewers from processing shared capture frames, targeting
  multi-viewer FPS degradation. Fixed connection readiness and ICE restart recovery.
- Allowed decoders without the optional low-latency interface/property to start.
  This improves older decoder compatibility; full Windows 7 support and the
  originally reported remote connection remain unverified.
- Added a Host column to the room list. The service sends host nicknames only to
  clients requesting the feature, preserving directory compatibility with 1.0.0.

Existing 1.0.0 clients receive this release through the built-in updater.
Choose Download Update, then Install and restart after leaving the room.
No experimental-update channel or manual ZIP transfer is required.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives remain
available as release assets. See docs/diagnostics.md for report collection and
docs/known-limitations.md for outstanding hardware and network qualification.
