# ScreenShare 1.0.6

- Fix arrow keys and other extended keyboard keys during remote control,
  including releasing held keys when control stops.
- Keep keyboard permission while a previously captured game window is minimized.
  Input pauses while the shared window is minimized or unfocused and resumes
  when it is visible and focused again.
- Fix capture event-queue ownership when switching shared sources.
- Promptly remove room membership after media failure, preventing stale viewers
  or reconnecting rooms from remaining in the room list until timeout.
- Show host-side input grant failures beside the affected viewer.

Automated checks cover key encoding, minimized-window grants without input
injection, capture dispatcher lifetime, room cleanup and reconnection. Full
Silksong sessions and two-computer keyboard input remain unverified.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
