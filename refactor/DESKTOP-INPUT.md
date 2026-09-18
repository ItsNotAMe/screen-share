# Mouse/keyboard integration

The opt-in v2 UI and CLI now support explicit mouse/keyboard permission alongside
controllers. This completes the local implementation group, not Stage 3/Gate D
physical acceptance or default cutover. Both endpoints must use input protocol v2;
there is no downgrade to v1 input. Media remains independent of input permission.

## Frame identity and confinement

Capture owns a monotonically changing source generation and its current bounds.
Each H.264 access unit carries bounded UUID-owned SEI metadata: source generation,
encoded dimensions and active image rectangle. It follows SPS/PPS and precedes
the first slice. No HWND, process ID or desktop coordinates are transmitted.
The decoder associates metadata with that exact sample, including delayed output;
missing, malformed, duplicate or mismatched metadata cannot authorize input.

Qt and the native CLI preview publish mapping only after successful presentation.
Queued, dropped or resized surfaces cannot substitute a newer mapping for the
displayed image. Pointer/button/wheel coordinates pass through both viewer
letterboxing and encoder padding; events in padding are rejected. Wheel events
carry their own position. Keys and pointer events carry the source generation.
The GPU wrapper retains native textures without adding a readback.

The host binds each desktop grant to the capture generation. Geometry/source
changes, unavailable capture, stale liveness, disconnect and backend failure revoke;
a new valid frame and explicit consent are required to resume. Source switching
revokes even when a replacement fails. Failed source candidates cannot take over
the previous source's identity property.

Windows window control checks a capture-specific HWND property, PID, current
bounds, foreground root, visibility and minimized state. Mouse events must fall
inside the selected window's client area and pass its point-occlusion check.
Window shares prohibit keyboard control. Display shares use the pinned output's
captured bounds. Property access failure disables window input without stopping
video. These checks fail closed; they do not make Win32 focus checks and SendInput
atomic. Physical focus/desktop/UIPI behavior still needs acceptance testing.

`DesktopSink` composes the existing gamepad sink and useful RemoteInputInjector.
Mouse and keyboard have exclusive ownership per capability; release affects the
owning peer. It preserves the service's bounded queues, watchdog and input thread.
No second transport, adaptation loop or service polling was introduced.

## User interaction

The existing room page offers Gamepad, Mouse, Keyboard (display only), and
Mouse + Keyboard. The viewer selects capabilities, checks consent and requests;
the host selects the exact peer, checks consent and grants. Desktop requests need
a valid presented mapping. The host's explicit window grant attempts to foreground
the selected window; failure denies the grant. Saved settings and admission never
grant input. Status shows pending/active/failure and revoke remains available.

Viewer focus loss, deactivation, source changes and release outside the image
revoke and neutralize held input. The existing Ctrl+Alt+Shift+F12 panic handler is
retained (global registration is best effort). Recovery never automatically grants.

CLI desktop input uses `--control-file PATH` and an enabled, focused preview;
`--gamepad` is required only for controllers. Write a fresh increasing sequence
after startup, using the same peer IDs shown in status, for example:

```json
{"sequence":1,"operation":"request","peer":"HOST_ID","consent":true,"capabilities":"mouse"}
```

The host writes a separate command to its own control file:

```json
{"sequence":1,"operation":"grant","peer":"VIEWER_ID","consent":true,"capabilities":"mouse"}
```

Capabilities are `gamepad` (default), `mouse`, `keyboard`, or `mouse-keyboard`.
`revoke` needs no consent. Existing bounded-file, startup-ignore, peer validation
and consumed-command rules remain. CLI window grants require the target already
foreground; they do not activate arbitrary windows from a file poll.

## Verification and remaining scope

`DesktopInputTests` covers metadata escaping/truncation/duplicates, SPS ordering,
padding, exclusive ownership, stale generations, geometry invalidation, window
keyboard denial and backend failure using injected devices. Actual UI and CLI
scenarios send through encoded video and encrypted input channels into recording
sinks, including controller-to-mouse handoff, focus release, regrant and stale-frame
rejection. Native preview messages target only the test-owned HWND. Headless Qt uses
a recording presenter; the Windows extension uses real WGC capture and D3D display.
Audio is synthetic/discarded, and tests never fall back to physical input injection.

The runner contains 11 headless cases and 15 with `--desktop`. See
[HEADLESS-TESTING.md](HEADLESS-TESTING.md) for final artifacts. Integration exposed
two issues: offscreen D3D cannot prove presentation, so its test needs an injected
presenter; leading SEI hid resize SPS telemetry, fixed by inserting after headers.
Neither failed run is counted as a pass.

Next: Stage 4 resource/stress and impairment work, while retaining physical
mouse/keyboard/controller, media, two-machine/NAT/TLS and external input-to-photon
acceptance gates. The 4 ms input owner and bounded queues are implementation
properties, not measured gaming latency. Stage 2 parity/reporting audit, Stage 5
legacy removal/cutover and the later Stage 6 visual redesign remain open.
