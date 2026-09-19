# Controller integration

## GameSir Bluetooth field finding (2026-09-19)

The user's Nova Lite identifies as `DualShock 4 (native HID)` on the laptop.
The production reader enumerated it but returned zero valid states in 439 reads.
Read-only report inspection found report ID 0x11 delivered in a 547-byte buffer;
the first 78 bytes pass the Bluetooth CRC. The parser previously required the
entire read to be exactly 78 bytes, rejecting these padded buffers. DS4 0x11 and
DualSense 0x31 now parse their 78-byte payload before checking CRC. Truncation and
bad CRC still fail; no fallback disables integrity validation.

Release and Debug parser tests cover padded packets, unchanged decoded controls,
corruption and truncation. `ViewerGamepadReportTests --device-probe` performs a
five-second read-only check per detected controller, printing valid/missing counts
without HID paths or button contents. After reconnecting the controller, the
fixed laptop viewer successfully requested controller-only access from the desktop
host. The user confirmed that buttons/sticks work in the streamed test scene.
A host sample records 797 applied reports, zero rejected reports, an active
gamepad grant and no input error. An earlier session ended at its configured
30-minute deadline after 254 applied reports; that shutdown was distinct from
the fixed HID rejection. Raw logs are in `build/physical-controller-check/`
(the earlier session is preserved in `first-session/`).

This validates physical GameSir Bluetooth delivery through the native DS4 HID
reader and host virtual controller. The user also confirmed that releasing control
while holding a button clears the input cleanly. The host subsequently reports
1,373 applied reports, zero rejected, zero granted capabilities and no pending
release acknowledgement. Explicit held-button release passes for this setup.
The user then confirmed that turning off the controller while holding a button
clears the pressed input and disables control. The host records 1,592 applied
reports, zero rejected, zero granted capabilities and no pending release.
Disconnect cleanup passes for this setup. One intervening regrant was revoked
without new reports; refreshing/reselecting the controller allowed the next grant
to succeed. That intermittent regrant behavior is not explained by the successful
disconnect check and remains a follow-up. External input-to-display latency and
physical compatibility with other Xbox or Sony models remain unverified.

Follow-up: `ViewerGamepadReportTests --lifecycle-probe` keeps one selected device
across ten fresh polling threads, separated by 750 ms, without refreshing or
injecting input. The laptop returned `firstMissing=0 valid=263 missing=0` across
ten cycles. This rules out a deterministic failure in that exercised reader
lifetime pattern, but does not exercise permission messaging or close the
intermittent regrant observation.

The opt-in v2 room UI and CLI now support explicitly authorized controllers.
Mouse/keyboard control is now integrated; see [DESKTOP-INPUT.md](DESKTOP-INPUT.md).
Stage 3/Gate D and physical acceptance
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

The subsequent desktop-input group implements source identity, displayed-frame
mapping, letterboxing, pinned-window checks and explicit UI/CLI capability consent.
See DESKTOP-INPUT.md; the current matrix is 11 headless / 15 desktop-inclusive.
Physical pad compatibility, real driver behavior, physical desktop confinement
and external gaming latency remain acceptance work.
