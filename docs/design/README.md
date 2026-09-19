# ScreenShare 1.0 frontend direction

Status: design concepts, not implemented UI. Version 1.0.0 is the next unreleased
application version; no release, update manifest or deployment was published.

## Boards

- [Home and Create](01-home-create.png)
- [Join and Viewer](02-join-viewer.png)
- [Host and Room Settings](03-host-settings.png)
- [Preferences, Updates and recovery](04-preferences-updates.png)

Generated with the built-in image generation tool. The exact prompts are below.
The boards communicate visual hierarchy; the rules here override generated text,
sample metrics, dates, links and inconsistent navigation.

User corrections (authoritative over the original boards/prompts): keep profile
and Settings in the top title bar on every main screen; nickname editing is only
available through those top-bar menus, never in Create or Join. Preserve the exact
existing logo from `assets/brand/screenshare-logo.svg`, not a generated substitute.
Every host viewer row has three independent mouse, keyboard and controller icon
buttons to grant/revoke that capability directly, without a prior viewer request.

## Implementation contract

Keep the existing Qt frontend and modular session API. Reuse the application shell,
video presentation, controller consent and verified updater services. Replace the
temporary browser/session forms as complete flows, then delete their superseded
presentation code. Do not create a second permanent UI or duplicate backend state.

Use shared semantic tokens: background #0C1110, surface #151D1B, border #293631,
primary #38D8C8, text #EDF5F2, secondary #A3B5AF. Use flat surfaces, 8px corners,
Segoe UI/platform fallback and consistent spacing. Check actual contrast and focus
in the implemented controls; generated screenshots are not accessibility evidence.

Navigation: one consistent title bar with Home/back, profile and Settings. Avoid
the extra global sidebar invented in board 2. A local category list is useful in
Settings only. Profile, reached from the top-bar menus, owns nickname editing.
Create and Join consume the saved profile nickname without an editable field or
per-room override. Remove the Home footer nickname editor too. Keep profile and
Settings actions in the title bar, including during host/viewer sessions.
Avoid the large marketing headline in board 1 when space is limited.

Home displays live public rooms, stable identities, empty/offline/reconnecting
states and a discreet update entry. Private/unlisted rooms must NEVER appear in
the public directory (the private row on board 1 is a generation error). Password
protection and directory visibility are separate. Search filters current content;
background events must not reorder a row under the pointer. Use actual supported
invite formats, not the invented URL or short room ID in the images.

Create provides room name, visibility, optional password, viewer limit,
display/window selection, Gaming/Quality preset, resolution, FPS, bitrate and audio.
Create the invite only after successful room creation. Keep advanced settings
collapsed. Explain exact existing manual resolution/FPS/bitrate policy near those
controls; distinguish configured target/ceiling from actual output and do not
promise constant delivered bitrate. Include audio device/process selection where
supported. Preserve drafts and show inline failures.

Host shows source, sharing status, audio and per-viewer controls. Each viewer row
always exposes mouse, keyboard and controller icon toggle buttons. Clicking an
off button grants that capability directly; clicking an on button revokes it.
A viewer request is optional, never a prerequisite for the host action. The
explicit host click is consent for that peer/capability; do not add a redundant
host consent checkbox. Keep other granted capabilities when toggling one, subject
to backend ownership rules. Use acknowledged permission state, pending/failed
feedback, accessible pressed state and tooltips such as "Grant mouse to Maya" /
"Revoke mouse from Maya". Explain unavailable capabilities without hiding them.
Requests can highlight the corresponding button and still support denial.
Include release-all and the existing panic
shortcut. Window-capture keyboard restrictions remain visible. Show per-peer RTT
and actual stream statistics in Details; RTT is not input-to-image latency.
Source previews, pause and additional thumbnail UI are design proposals: bind only
supported capabilities, or implement and test the behavior before exposing them.

Viewer prioritizes the video canvas and a collapsible controls panel. Controller
selection, explicit consent, request/pending/granted/released states and release
action remain clear. Keep local mute/volume/output, fullscreen and Leave reachable.
No floating native video over scrolled controls. Fullscreen uses a discoverable
toolbar with keyboard access. Disconnect/focus behavior must preserve input safety.
Never imply a selected controller automatically grants access.
Host-initiated grants must work through the entire input lifecycle: the current
viewer code rejects grants when it has not armed a matching request, so changing
host buttons alone is insufficient. Separate viewer local input consent/device
readiness from requesting permission; accept host grants without requiring a
request while respecting the viewer's local opt-out. Display "Host granted
control" and any local readiness requirement clearly. Verify unsolicited grants,
independent capability toggles, revoke/regrant and disconnect/focus release.

Room settings separates Stream and Room. Keep pending/applying/applied/error states
and stable Cancel/Apply actions; changing a selector is not proof of backend success.
Diagnostics/report export lives under Details/Help rather than the main create form.

Updates must include checking/current/available/downloading/verifying/ready/error
states, release notes, retry and Later, installed/portable package selection and
existing signature/hash validation. Add a manual Check now action. Automatic checks
must not open an interrupting dialog during streaming. Installation/restart waits
for the user to end the active session and explicitly install; do not disconnect
a session or restart automatically. The 1.0.1 update and dates are fictional examples.
The current updater does not yet implement all these proposed presentation/lifecycle
states; preserve its security logic while implementing them.

## Resize and interaction acceptance

At compact 800x600, stack create fields and collapse optional host/viewer panels;
use one intentional form scroll area and keep primary actions reachable. At wider
sizes use two columns and bounded reading widths. At 150–200% text scale adapt
layout rather than clipping. Preserve drafts, selection, focus and session state
while resizing. Native keyboard traversal, named icon buttons, visible focus,
Escape/return-focus for drawers and dialogs, and non-color status labels are required.
Keep room-list content during reconnect, prevent duplicate actions, reject stale
async results, and keep transient feedback from moving controls.

Verify rendered Home/Create/Join/Host/Viewer/Settings/Updates at compact and wide
sizes with deterministic fixtures. Exercise empty, password error, request/revoke,
reconnect, update failure and active-session update deferral. No claim of runtime
layout, accessibility or updater lifecycle validation is made by these images.

## Delivery and final cleanup

1. Implement shared shell/tokens plus Home/Create/Join as one coherent group.
2. Implement Host/Viewer/settings/control lifecycle and preferences/updates as a
   second coherent group; remove the superseded temporary UI as replacements land.
3. Validate affected frontend flows, packaging and update handling, then consolidate:
   move every unfinished accepted item to agents/todo.md; retain only durable user,
   build, security, architecture and concise comparison/known-limit documentation.
   Remove refactor/ and obsolete Markdown/JSON artifacts after important material
   and required fixtures have been migrated. Update links, scripts and tests before
   deletion. Do not delete runtime configuration, manifests, test fixtures or
   licensing just because their extension is JSON/Markdown.
   Keep these design boards only while useful; avoid archiving the entire campaign.
   Complete this cleanup before the eventual main merge. Publishing remains separate.

## Generation prompts

These are historical reproduction prompts for the first draft. The user
corrections and implementation contract above supersede conflicting details.

### Board 1

Use case: ui-mockup. Generate a high-fidelity design board for ScreenShare v1.0.0, a native Windows screen-sharing and remote-gaming desktop app. Exactly two large app windows arranged vertically on a spacious portrait canvas, front-on flat screenshot style, highly readable crisp UI text, no perspective or device frames. Small outside screen labels only. Consistent design system: almost-black green charcoal background #0C1110, surface #151D1B, subtle borders #293631, bright teal #38D8C8, off-white #EDF5F2 and readable muted sage-gray #A3B5AF. Segoe UI-style sans-serif, restrained 8px corners, generous coherent spacing, thin outline icons, compact Windows title bars with minimize/maximize/close. Brand is ScreenShare and a teal overlapping-monitors icon. Professional useful desktop interface, not a website, no gradients, no huge promotional titles, no decorative charts. Teal for primary action, restrained red for destructive actions, status text plus icon. All controls inside window bounds. This is a redesign of a simple teal/charcoal legacy app, preserving identity while improving hierarchy. Example metrics and room names are illustrative. Board 1: GET STARTED. Top window HOME: header ScreenShare with v1.0.0 small badge, right profile Ofek and Settings gear. Main title 'Share a screen. Stay connected.' Two balanced action cards 'Start sharing' with subtitle 'Choose what others see' and 'Join a room' subtitle 'Connect with a room link'. Below 'Available rooms' with small green dot 'Live', search field; two clean room rows 'Ofek’s gaming room' host Ofek, 1 viewer, Gaming badge and Join button; 'Design review' host Maya, lock Private and Join. Footer subtle 'Your nickname: Ofek' edit and 'Update available' text link. Bottom window CREATE ROOM: back Home, title 'Create a room'. Two columns: left Room details fields Room name 'Friday games', Nickname 'Ofek', visibility segmented Public / Private, Password optional field. Right 'Share source' two tabs Display / Window and two understated source thumbnail choices Display 1 selected, Display 2. Below right preset segmented Gaming selected / Quality; compact two-column fields Resolution Auto, Frame rate 60 FPS, Bitrate Auto, Shared audio System audio. Collapsed 'Advanced settings'. Hint 'Gaming prioritizes responsiveness.' Footer Cancel and prominent 'Create & start sharing'. No invite link before creation. Form labels visible, controls aligned, enough whitespace. Render two complete windows with legible text.

### Board 2

Use case: ui-mockup. Generate a high-fidelity design board for ScreenShare v1.0.0, a native Windows screen-sharing and remote-gaming desktop app. Exactly two large app windows arranged vertically on a spacious portrait canvas, front-on flat screenshot style, highly readable crisp UI text, no perspective or device frames. Small outside screen labels only. Consistent design system: almost-black green charcoal background #0C1110, surface #151D1B, subtle borders #293631, bright teal #38D8C8, off-white #EDF5F2 and readable muted sage-gray #A3B5AF. Segoe UI-style sans-serif, restrained 8px corners, generous coherent spacing, thin outline icons, compact Windows title bars with minimize/maximize/close. Brand is ScreenShare and a teal overlapping-monitors icon. Professional useful desktop interface, not a website, no gradients, no huge promotional titles, no decorative charts. Teal for primary action, restrained red for destructive actions, status text plus icon. All controls inside window bounds. This is a redesign of a simple teal/charcoal legacy app, preserving identity while improving hierarchy. Example metrics and room names are illustrative. Board 2: JOIN AND WATCH. Top complete window 'Join a room', back Home. Primary room link field with Paste and Join room actions. Nickname Ofek. Below 'Available rooms' and Live indicator, two rows Friday games / Ofek / 1 viewer and Design review / Maya / Password required, each Join button. No private/unlisted rooms in public directory. Collapsed 'Playback options' for decoder/output preferences. Small inline password entry panel for selected password-protected PUBLIC room, with Cancel and Join. Bottom large complete window active VIEWER 'Friday games', Connected indicator. Dominant clean 16:9 video canvas showing a tasteful racing game scene. Compact header room name, connection icon, panel toggle. Right collapsible 'Controls' panel: 'Controller' selector 'GameSir • DualShock mode', detected indicator; checkboxes Keyboard, Mouse, Controller (only controller selected); 'Request control' teal button; text 'The host approves each request'. Lower panel 'Connection' RTT 12 ms, 1080p, 60 FPS, bitrate 8.4 Mbps, clear metrics are illustrative network RTT not end-to-end image latency. Fixed bottom toolbar volume/mute, fullscreen, Controls, red subtle Leave room. Viewer video takes most room width, no giant dashboard; no scroll across video. Keep same darker charcoal teal native desktop style.

### Board 3

Use case: ui-mockup. Generate a high-fidelity design board for ScreenShare v1.0.0, a native Windows screen-sharing and remote-gaming desktop app. Exactly two large app windows arranged vertically on a spacious portrait canvas, front-on flat screenshot style, highly readable crisp UI text, no perspective or device frames. Small outside screen labels only. Consistent design system: almost-black green charcoal background #0C1110, surface #151D1B, subtle borders #293631, bright teal #38D8C8, off-white #EDF5F2 and readable muted sage-gray #A3B5AF. Segoe UI-style sans-serif, restrained 8px corners, generous coherent spacing, thin outline icons, compact Windows title bars with minimize/maximize/close. Brand is ScreenShare and a teal overlapping-monitors icon. Professional useful desktop interface, not a website, no gradients, no huge promotional titles, no decorative charts. Teal for primary action, restrained red for destructive actions, status text plus icon. All controls inside window bounds. This is a redesign of a simple teal/charcoal legacy app, preserving identity while improving hierarchy. Example metrics and room names are illustrative. Board 3: HOST AND ROOM SETTINGS. No sidebar navigation outside in-session controls. Top window HOSTING Friday games, green Sharing, elapsed 12:34. Left large 'You’re sharing Display 1' card with tasteful thumbnail of racing game, 1920 × 1080 / 60 FPS / Gaming. Below toolbar Change source, Pause video, Mute audio. Under it compact Stream health Good, Actual bitrate 8.4 Mbps, Network RTT 12 ms, Frames sent 60 FPS, Details disclosure. Right 'Viewers · 2' two peer rows Maya Watching and Alex 'Requests controller access'; Alex row buttons Allow controller / Deny. Existing grant on Maya is absent (watch-only). Under list explanatory 'Only approved controls can be used'. Bottom pinned footer Room settings, Copy invite, outlined red Stop sharing. Bottom second full host window with right ROOM SETTINGS drawer 45 percent width and dimmed host background. Drawer tabs Stream / Room. Stream selected, Preset Gaming, Source Display 1, Resolution Auto, Frame rate 60 FPS, Bitrate Auto; Shared audio System audio, Change audio device link, collapsed Advanced. Tiny policy hint 'Auto adapts to the connection. Manual options show their limits.' Sticky Cancel / Apply changes footer. Include a small clear status 'Changes apply while sharing' rather than pretending every setting applies immediately. All controls within bounds; no unnecessary scrollbars, no giant red button. Match clean native titlebar, teal identity, dark near black surfaces.

### Board 4

Use case: ui-mockup. Generate a high-fidelity design board for ScreenShare v1.0.0, a native Windows screen-sharing and remote-gaming desktop app. Exactly two large app windows arranged vertically on a spacious portrait canvas, front-on flat screenshot style, highly readable crisp UI text, no perspective or device frames. Small outside screen labels only. Consistent design system: almost-black green charcoal background #0C1110, surface #151D1B, subtle borders #293631, bright teal #38D8C8, off-white #EDF5F2 and readable muted sage-gray #A3B5AF. Segoe UI-style sans-serif, restrained 8px corners, generous coherent spacing, thin outline icons, compact Windows title bars with minimize/maximize/close. Brand is ScreenShare and a teal overlapping-monitors icon. Professional useful desktop interface, not a website, no gradients, no huge promotional titles, no decorative charts. Teal for primary action, restrained red for destructive actions, status text plus icon. All controls inside window bounds. This is a redesign of a simple teal/charcoal legacy app, preserving identity while improving hierarchy. Example metrics and room names are illustrative. Board 4: PREFERENCES AND AUTO-UPDATE. Top full window ScreenShare v1.0.0, back Home, title Settings. Compact settings-only left navigation Profile, Playback, Updates (selected), About. Main content Updates, 'Current version 1.0.0', toggle 'Check for updates automatically' enabled, quiet last checked time. Single update card 'Version 1.0.1 is available', short release notes Better connection recovery / Interface improvements, primary Download update and secondary Later. Reserved status area note 'Installation waits until you finish sharing or watching.' Bottom of settings window small inline nickname preference 'Display name: Ofek' explaining visible to other room members. Bottom board area is SECOND full window titled 'Update and connection states — design reference' containing four clearly separated state examples in a 2x2 layout (this is a design component sheet, not one confusing live app). Card1 'Downloading update' 64% progress bar, Cancel. Card2 'Ready to install' verified package check icon, 'Finish your session before restarting', disabled Install & restart and Later. Card3 'Couldn’t download the update', 'Your current version still works.', Retry. Card4 'Reconnecting…', 'Your controls are released while disconnected.', spinner, Leave room. Include a quiet 'Up to date' status sample underneath and focus outline on Retry to demonstrate keyboard access. All version1.0.1 update details hypothetical design examples, no mock performance claims. Exact darker teal design from other boards, crisp legible UI, no invented cloud account or subscription, no website promotional copy.
