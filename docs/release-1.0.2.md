# ScreenShare 1.0.2

- Fixed room disruption when a viewer leaves. Late signaling for a departed
  viewer no longer disconnects the host or interrupts the remaining viewers.
- Redesigned room controls: click a mouse, keyboard or controller icon to request
  access immediately. Amber indicates a pending request; teal indicates granted
  access. Controls start unselected, and the host can allow or deny each control
  independently. Requesting another control preserves existing permissions.
- Improved host and viewer Details with a readable Overview for connection,
  video and audio, clearer units and measurement availability, and a separate
  Advanced tab for full diagnostics. Fixed the encoder target bitrate units.
- Added an always-available Open log folder action, including before a report
  has been saved. Log actions remain visible while details scroll.

Update both the host and viewers to use the complete independent request and
denial feedback flow. Older viewers ignore the new per-control denial feedback.
Existing installations receive this release through the built-in updater;
install and restart after leaving the room.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
