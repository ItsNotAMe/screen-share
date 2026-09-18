PRIVATE TEST BUILD — not the default/released backend

Extract the full folder on both PCs. Run Start-V2.cmd on both.
Use your normal devices; this package installs no driver and changes no firewall rules.
The isolated v2 signaling service must be deployed before rooms work.

FIRST PASS (about 5 minutes, same network)
1. Host PC: open ScreenShareFieldScene.exe. Start Sharing in the app:
   select the test-scene window, Gaming, 1080p60, and No shared audio.
2. Laptop: Join Room using the host's room ID. Resize the viewer once.
   Check motion, text and colors. Minimize/restore the host test-scene window:
   sharing should pause/recover without revealing another window.
3. Connect the GameSir to the laptop in its Windows/XInput mode. In the viewer
   select it, check consent, and request Gamepad. Host: select that viewer,
   consent and grant. Press buttons/move sticks: the scene should show them.
   While holding a button, revoke on the host: it must release/disappear.
   Request/grant again, then unplug the laptop controller: it must release.
   If the host reports a missing virtual-controller driver, record that result;
   do not install anything just to hide it. Video should continue.
4. Save diagnostic reports on BOTH PCs. Report only: video/privacy pass or fail;
   controller grant/revoke/unplug pass or fail; any error text; wired or Wi-Fi.

SECOND PASS (after the first works)
- Host shares the DISPLAY containing the test scene; foreground the scene.
  Laptop requests Mouse + Keyboard; host consents/grants. Hold Space and click
  inside the image; the marker turns green. Release/revoke/focus away: it clears.
  Never test control against unrelated apps. Window sharing intentionally denies
  keyboard control. A source switch must revoke and require fresh consent.
- Enable system audio with your own short clip, then microphone. Check quality,
  volume/mute and switch/unplug/replug. A failed audio endpoint must not stop video.
  This scene generates no sound. Mark untested surround/HDR/device cases unknown.
- If practical, repeat joining with the laptop on a phone hotspot. Note success
  or direct-connect failure; direct-only operation cannot promise every NAT works.

LATENCY (separate acceptance, not needed for the first pass)
Film both displays together at a known high frame rate, with the host clock
visible. For input response, include the physical button and displayed marker.
Keep raw video and recording FPS; collect at least 100 independent observations
per mode before claiming p50/p95/p99. The UI clock/counters and one local sample
are not external latency measurements. We will analyze recordings separately.

An Xbox-style host virtual pad does not prove every physical Xbox or PlayStation
device. GameSir evidence covers that device/mode only; DualShock 4/DualSense paths
need their own devices for physical acceptance.
