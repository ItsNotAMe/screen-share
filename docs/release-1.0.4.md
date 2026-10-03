# ScreenShare 1.0.4

- Enabled remote keyboard control when sharing a specific window. Hosts can
  grant keyboard access directly or accept a viewer's keyboard request.
- Window mouse and keyboard input pauses while the shared window is not focused.
  Held keys and mouse buttons are released, and permissions are retained so new
  input resumes when focus returns. Input received while unfocused is discarded.
- Updated control tooltips to explain window focus behavior.

Update the host to enable keyboard control for window sharing. Source changes,
disconnects and stale input retain their existing release safeguards.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
