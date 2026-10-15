# ScreenShare 1.0.11

- Choose a display or window in a larger source picker, shared by room creation
  and the host room. Larger thumbnails, full-title tooltips and explicit
  confirmation make sources easier to identify.
- Keep a live preview of the selected source on the create-room form.
- Keep the room and existing viewers connected when the shared window closes.
  Video pauses until the host uses **Change source** to select a replacement.
- Fix preview image memory ownership that could cause crashes when capture and
  preview sizes match.
- Make the update dialog resizable, keep long release notes scrollable, and keep
  download progress and actions readable at smaller sizes and display scaling.

Validation includes the application regression suite, native window closure and
replacement with two viewers, a 30-second hardware-encoded two-viewer run, and a
60-second synthetic four-viewer run with a slow consumer. These checks do not
establish that every reported sharing crash is resolved; the reported Miyomu
sharing crash has not been reproduced on the affected machine.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
