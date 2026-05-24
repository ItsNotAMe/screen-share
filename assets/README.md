# ScreenShare Assets

This folder holds design and UI asset drafts.

- `design/revamped-ui-draft-2026-05-25.png`: current stage-2 UI direction mockup.
- `brand/screenshare-mark.svg`: square app/logo mark source.
- `brand/screenshare-logo.svg`: horizontal logo/wordmark source.
- `ui/icons/`: first-pass button icon set for the revamped native UI.

The SVG icons use `currentColor` so the UI can tint them for normal, hover, disabled, and danger
states when they are wired into the application.
The current Qt UI embeds the brand mark and button icons through `src/ui/resources.qrc`.
