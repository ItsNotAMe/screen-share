# Capture fallback, cursor and lifecycle — 2026-09-18

The Windows v2 capture implementation now supports display-only fallback and
source-preserving rebuilds. This completes the implementation batch, not physical
capture/driver acceptance or Stage 2 as a whole.

## Contracts

- `WindowsCaptureSource` enables WGC-to-DXGI fallback for display startup only.
  `CaptureBackendPolicy.h` allowlists unavailable-interface/implementation errors.
  Access denial, device removal, arbitrary failures and window sources never
  trigger fallback. A failed DXGI start is terminal; it is not a retry loop.
- Display selection pins the original DXGI adapter/output and checks its monitor,
  device name and attached state. Fallback/rebuild never reenumerates an index.
  WGC recovery retains the original capture item and closure subscription; window
  recovery never recreates an item from a potentially reused numeric HWND.
- Partially initialized WGC resources close before DXGI startup. DXGI access loss
  and recognized D3D device failures enter the existing retirement/backoff policy:
  three rebuilds, generation changes and cancellable waits. Published buffers on
  the retired device remain invalid. Rebuild cannot revive the old encoder device.
- Window minimization produces a public `minimized` state, discards queued source
  pixels and pauses first-frame startup deadlines. Restoration resumes on fresh
  output. Source closure is `source-closed`, including closure during acquisition
  or reconstruction, instead of a generic source failure. The switchable-source
  and Windows runtime wrappers forward these states.
- A replacement selected while minimized still needs a valid first frame within
  the existing five-second source-switch transaction; otherwise the previous
  source remains selected. This differs intentionally from pausing an already
  selected window. Candidate minimized pixels cannot commit a source switch.
- Local UI/CLI pipeline diagnostics report `captureBackend` (`wgc`, `dxgi`, or
  `unknown`) and `captureFallback` (boolean, or null when unknown). Observations
  follow a committed source switch. No server polling or signaling is added.

## Cursor, color and supported geometry

`DxgiCursor` is a separate capture-owned module. It reads only bounded pointer
shape metadata (at most 1024×1024 visible pixels / 4 MiB), not desktop pixels.
Color, monochrome AND/XOR and masked-color cursors are composed on the GPU after
the existing HDR-to-SDR/size conversion and before NV12 conversion. Position is
mapped from source to output dimensions, including negative-edge clipping.
Shape textures are reused until the pointer shape changes; background/output
textures are reused until dimensions change. Device rebuild discards all caches.
Visible cursor composition costs a GPU copy and draw; this is not zero-copy or a
measured latency improvement.

DXGI rotation is explicitly rejected until correctly rotated capture/input
coordinates are implemented. WGC remains the preferred backend for rotated
displays. Owned HDR duplication must negotiate a native HDR format; unsupported
formats fail explicitly instead of silently using the old BGRA approximation.
The API format list includes BGRA as required by
[Microsoft's DuplicateOutput1 contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1).
Physical HDR color/brightness acceptance remains open.

## Tests and limits

- Release/Debug `CaptureBackendTest` validates fallback allow/deny policy and
  actual WARP-rendered pixels for straight alpha, monochrome truth table,
  masked-color XOR, negative coordinates, invisibility and scaled placement.
- Release/Debug `CaptureSessionTest` covers 100 restarts, three rebuilds then
  terminal exhaustion, cancellation, startup timeout, minimized startup without
  delivery, restoration, and closure during polling/rebuild.
- Release/Debug `HostMediaSessionTest` covers wrapper/coordinator state propagation,
  backend diagnostics across source replacement, and source-closed classification,
  in addition to its membership, cancellation and restart checks.
- Both complete silent headless room matrices pass **5/5**:
  `build/webrtc/capture-headless-{release,debug}/result.json`.
- `CaptureBackendTest --live` passes in both builds using only a generated window:
  minimize before first frame, restore, minimize after delivery, close and reject
  window-to-DXGI capture. `CaptureRecoveryTest --live` passes in both builds.
- Release `CaptureBackendTest --display` rebuilds the pinned WGC display and
  verifies owned frames without CPU readback or saved pixels. DXGI returned
  `DXGI_ERROR_UNSUPPORTED` on the current desktop; its rebuild was **not** passed.
  Windows was subsequently confirmed to be on the `Screen-saver` input desktop,
  so this result must not be interpreted as proof of a permanent driver limit.
- The desktop-inclusive Release matrix passed its first five cases, then native
  CLI presentation reported occlusion. Debug reproduced the same condition.
  Geometry confirmed an on-monitor visible, non-minimized test window, while a
  read-only desktop query found `Default` versus `Screen-saver`. The regression
  runner now records this condition as **blocked**, never passed, before launching
  desktop scenarios. It does not dismiss the saver or inject input.

Evidence: `build/webrtc/capture-{build,app}-{release,debug}.log`,
`capture-live-{release,debug}.log`, `capture-display-release.log`,
`capture-verified-release/result.json`, `capture-cli-geometry/`, and
`capture-desktop-preflight/result.json`. Focused CTest results are also retained
in each proof build's `Testing/Temporary/LastTest.log`.

Remaining acceptance: rerun both full desktop matrices on an available interactive
desktop; exercise real DXGI fallback/rebuild on supported SDR/HDR outputs; physical
hotplug, forced HWND reuse, multi-adapter changes, driver removal/hangs, protected
content, cursor/HDR visual acceptance and latency/resource measurements. The
screensaver explains this observed occlusion; it does not establish the cause of
every historical occlusion report.

Next implementation group: ordinary UI/CLI adoption with existing feature parity.
Gaming input remains Stage 3; default cutover and deletion remain Stage 5 gates.
