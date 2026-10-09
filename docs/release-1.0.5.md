# ScreenShare 1.0.5

- Window sharing now defaults to the selected app's audio instead of all system
  audio. Display sharing continues to capture system audio.
- Shared source audio follows window/display changes and mute/unmute. System,
  microphone, process and no-audio modes remain available in the audio settings.
- Unavailable window audio stays silent while video continues, without falling
  back to all system audio.

Windows captures audio from the selected app process and its child processes,
so other windows or tabs in the same app may also be audible. App audio requires
Windows build 20348 or later, including Windows 11.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
