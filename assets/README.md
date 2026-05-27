# ScreenShare Assets

This folder holds brand and UI assets.

- `brand/screenshare-mark.svg`: square app/logo mark source.
- `brand/screenshare-logo.svg`: horizontal logo/wordmark source.
- `brand/screenshare.ico`: Windows executable icon generated from the mark.
- `ui/icons/`: first-pass button icon set for the revamped native UI.

The brand SVGs carry fixed ScreenShare identity colors. The UI icons use `currentColor`; the Qt UI
renders them through QtSvg with explicit theme colors for normal, hover, checked, and disabled
button states.
