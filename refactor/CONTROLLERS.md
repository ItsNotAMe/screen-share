# Controller integration

The opt-in v2 room UI and CLI now support explicitly authorized controllers.
Mouse/keyboard control remains disabled. Stage 3/Gate D and physical acceptance
remain open; default backend selection and release gates have not changed.

## Ownership and safety

`GamepadSink` owns per-peer virtual devices on the input service thread. Creation
is lazy, after a host grant. It reserves at most three remote pads, checks occupied
XInput slots, and verifies the actual user index before accepting a device. A
missing driver, missing index API, collision or failed update denies/revokes that
peer without stopping media. Release neutralizes only the owned device. Runtime
code never installs or repairs drivers. The client DLL is loaded only from beside
the executable, with system dependencies; current-directory/PATH lookup is excluded.
The queried API is [ViGEm's x360 user-index API](https://github.com/nefarius/ViGEmClient/blob/master/include/ViGEm/Client.h),
not its target serial number. Real-driver allocation still needs physical validation.

`GamepadPoller` reads only the explicitly selected viewer device on a dedicated
4 ms loop and submits changed full states. The existing service supplies 100 ms
full-state keepalive and the 300 ms watchdog. Unplug, invalid reports and submission
failure revoke; it never switches to another device automatically. These periods
are scheduling bounds, not measured input-to-photon latency.

Driver callbacks run outside the public service mutex. Revoke/status stay responsive
during slow creation; the permission epoch is rechecked before publishing a grant.
A revoked late device is immediately released. Native driver calls cannot be
preempted, and shutdown must still wait for an in-progress driver call to return.
Repeated viewer cleanup preserves an unsent release. A new request is blocked
until the host acknowledges revocation, preventing an old acknowledgement from
cancelling fresh consent. This race has a dedicated service regression.

## UI workflow

The viewer refreshes the device list, selects a controller, checks consent and
requests control. The host selects the exact peer, checks consent and grants the
request. Consent is consumed per grant and cleared on peer changes; saved profiles
and room admission do not imply consent. Indicators show pending, active and failed
states. Host controls revoke one peer or everyone. Ctrl+Alt+Shift+F12 uses the existing
AppShell panic handler (existing best-effort registration; in-app revoke remains
available if another application owns the shortcut). Viewer deactivation/hide, unplug, source change and disconnect
release control; recovery requires a new request/grant. Host focus may move to the game.

## CLI workflow

The v2 create/join command parser accepts `--control-file PATH`; viewers also require
`--gamepad DEVICE_ID` and an enabled preview. These are command-line options, not
persisted profile or JSON configuration fields. Use a private local file and replace
it atomically with a new command. Polling is local at 100 ms, with no server requests.
Existing content at startup is ignored. Commands have strictly increasing integer
`sequence` values (1 through 2^53-1), exact allowed keys and a maximum size of 4096 bytes.

```json
{"sequence":2,"operation":"request","peer":"HOST_PEER_ID","consent":true}
```

The host writes `operation:"grant"` with the requesting viewer's exact peer ID and
`consent:true`. Either endpoint writes `operation:"revoke"`; omit `peer` to revoke all.
Status JSON exposes input readiness, requests, grants, pending state and reason.
An `input-command` result's `accepted` means local queue acceptance, not remote grant.
Invalid/unavailable commands are consumed, never automatically retried. A new
sequence and fresh explicit consent are required after failure/revoke. Preview focus
loss cancels local arming as well as remote permission. The CLI registers the same
panic shortcut; failure to register aborts a control-enabled launch.

## Silent verification and remaining work

`GamepadControlTests` injects virtual devices/readers to exercise local-slot preservation,
three remote pads, exhaustion, collision, missing backend, state changes and unplug.
`InputServiceTests` includes a blocked grant followed by responsive revoke and no
late revival. Actual room UI and CLI fixtures negotiate real channels and use injected
recording devices. The UI covers consent, changes, focus/panic/source revoke, regrant,
unplug and backend failure. The CLI uses fresh command-file requests/grants/revoke and
ignores startup commands. No test enumerates/injects real controllers as a fallback;
audio remains synthetic/discarded. Controller UI runs in a separate fresh service
fixture to avoid accumulating room creation budget across unrelated scenarios.
The regression runner now has nine headless cases and twelve with its generated-
window desktop extension. Production rate limits and test deadlines are unchanged.
Test windows show without activation; focus-loss is sent explicitly to the owned
viewer widget. Panic tests invoke the revoke handler, not a global keyboard shortcut.

Next: mouse/keyboard captured-source identity and displayed-generation mapping,
letterboxing, pinned-window confinement, foreground/occlusion/minimize protection,
window-share keyboard prohibition, explicit capability consent, and end-to-end
recording-sink UI/CLI tests. Preserve this controller implementation. Physical pad
compatibility, real driver behavior and external gaming latency remain acceptance work.
