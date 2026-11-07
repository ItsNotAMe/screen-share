# ScreenShare 1.0.13

- Keep control permissions through temporary capture/input gaps. Release held
  input while video is unavailable, then resume with fresh video and input.
- Request, cancel or release Mouse, Keyboard and Controller independently by
  clicking their indicators. Other permissions stay granted. Remove the separate
  viewer Release control button; Ctrl+Alt+Shift+Q remains **Release all**.
- Recover shared-source, system and microphone audio after interruptions, follow
  default-device changes and retry selected devices without substituting another
  microphone or output. Recover viewer playback when its buffer stops draining,
  preserving volume and mute settings.
- Keep the room and audio connected when capture fails. The host can use
  **Change source** to restore video. Bound missing frames after restoring a
  minimized window and renew recovery allowances after sustained healthy video.
- Reduce redundant video scaling, readback, format conversions and frame copies.
  Reuse software-decoder output storage and reduce capture work while no viewer
  receives video.
- Search source titles in the sharing picker and show three cards per row at
  normal widths, with fewer columns in compact windows.
- Cancel pending room joins, retain invites for retry and show clearer room
  availability, capacity, admission and closure explanations.
- Choose where diagnostic reports are saved. Improve native file-picker contrast,
  dark scrollbars and the viewer's compact control hints and shortcut reference.
- Fix a mouse-monitor shutdown race, tolerate completed temporary software-encode
  stalls and retry transient stream-setting rejections.

Update both ends to use individual control release. With an older host, the
viewer explains when individual release needs an update; release-all remains
available.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets. Existing hardware/driver qualification limits remain
documented in [known limitations](https://github.com/ItsNotAMe/screen-share/blob/v1.0.13/docs/known-limitations.md).
