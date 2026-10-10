# ScreenShare 1.0.10

- Add a **Game mouse** toggle for remote 3D camera control. After receiving mouse
  permission, click the video to capture. The viewer cursor stays hidden at the
  video centre while raw relative movement controls the game. Update both PCs
  to use this mode.
- Fix keyboard grants when starting with window sharing. Granting access keeps
  the host's current window focused; the host selects the game when ready. Input
  pauses while the shared game is unfocused or minimized.
- Forward Escape and Tab to the game with keyboard access. Use
  **Ctrl+Alt+Shift+F** to toggle viewer fullscreen and **Ctrl+Alt+Shift+Q** to
  release control. The Controls panel and capture reminders show these shortcuts.
- Release held input and unlock the viewer cursor when control ends, focus is
  lost, or the shared video closes. The first capture click does not click in
  the game.
- Skip isolated GPU capture deadline misses and recover from repeated stalls
  with bounded retries. Clarify when the host room has ended instead of telling
  viewers to rejoin an ended room.

Local protocol, input, room, capture and native UI regressions cover these paths.
Compatibility and playability still depend on the game and the connection;
these checks do not establish end-to-end gameplay or latency on two PCs.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
