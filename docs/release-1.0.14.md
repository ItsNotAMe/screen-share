# ScreenShare 1.0.14

- Restore a sharp host preview at up to 30 FPS, sized to the available screen pixels up to 2560 × 1440. Reuse the captured preview pixels and smooth mouse-wheel scrolling in the sharing source chooser.
- Match capture cadence to the active stream FPS. Reuse Direct3D completion queries and let encoding and scaling continue while capture waits for its frame to finish.
- Keep direct GPU input for hardware encoding after changing the shared source. The encoder starts the new source with a keyframe.
- Keep video flowing when a capture frame cannot be read. Drop stale frames in the viewer’s presentation queue and fit oversized sources within 3840 × 2160 while preserving aspect ratio.
- Apply the hardware and software encoder’s 60 FPS ceiling to live settings. Rooms started from the CLI or a config file can raise FPS above the initial setting within the configured capture ceiling.
- Start video while the host’s audio capture or the viewer’s audio output opens. Preserve more of a short burst of captured audio within the existing freshness limit.
- Keep admitted viewers connected while the host prepares the first offer. If the initial capture source fails, use **Change source** to start video in the same room. Signaling reconnect attempts share one 35-second recovery window.

Setup and portable packages use the existing signed update manifest. Qt source archives are included with the release assets.
