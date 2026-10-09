# ScreenShare 1.0.9

- Fix missing audio when sharing Miyomu's app window. Shared source audio now
  discovers the app's render sessions and targets their smallest common process
  subtree, including WebView2 audio helpers that Windows can omit when capture
  targets the main window process.
- Follow playback starting after sharing and audio helper restarts. Keep capture
  restricted to the selected app's live process family, check process identities,
  and discard pending audio when the target changes. A closed app remains silent.
- Add regression checks for helper selection, multiple render sessions,
  unrelated apps, recycled parent PIDs and source transitions.

Live Miyomu playback was verified through the shared-source PCM capture path;
samples were measured in memory without saving or replaying audio. The full
release suite also checks stereo levels, encrypted Opus transport, room
lifecycle, updates and native UI. This release preserves the stereo and audio
level fixes from 1.0.8. Apps rendering simultaneously in independent helper
branches still depend on Windows capturing their common ancestor; those
combinations need further qualification.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
