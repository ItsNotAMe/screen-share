# ScreenShare 1.0.7

- Fix the viewer dropping arrow keys and other extended keys before sending
  them. Convert Qt's native scan codes into the input protocol's existing format
  for presses and releases, preserving the distinction from keypad keys.
- Allow keyboard-only grants for a live captured window when its captured size
  differs from Windows' reported bounds or those bounds are unavailable.
  Mouse control still requires a verified coordinate mapping. Window identity,
  source generation, visibility and focus checks remain in place.
- Record input-target availability and window marker setup failures in saved
  diagnostic reports to help identify remaining grant failures.

Regression checks reproduce the previous arrow-event and window-grant failures.
They cover both video event paths, protocol encoding/decoding, Windows key
payloads, mismatched/missing window bounds, borderless and minimized capture,
source replacement and native room UI. Physical two-computer key delivery and
Silksong itself remain unverified.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
