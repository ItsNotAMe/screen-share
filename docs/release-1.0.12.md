# ScreenShare 1.0.12

- Mute or select host audio before the first viewer joins. New viewers respect
  the saved selection, and sharing audio restores the previous source.
- Remove an individual viewer without ending the room or interrupting others.
  The person-minus action sits beside the input controls and asks for confirmation.
- Show viewers when the host pauses video, minimizes the shared window, loses
  the source or restores capture. Clear stale pictures and release held input;
  remote input resumes after fresh video arrives while host permissions stay set.
- Remember the main window's size, position and maximized state across launches.
  Fullscreen and minimization remain temporary.
- Improve audio recovery after playback stalls and prioritize audio workers
  under CPU load. Add timing diagnostics to help investigate remaining stalls.
- Reduce preview capture/readback work and diagnostic snapshot frequency.
  Preserve room-list rows and keyboard focus, and make empty searches easy to clear.
- Explain full, closed, missing and rate-limited rooms, preserve invites after
  failed joins, and distinguish service capacity from a room's viewer limit.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
