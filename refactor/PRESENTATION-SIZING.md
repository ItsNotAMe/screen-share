# Native presentation dimensions — laptop finding

During the first two-PC test, the user confirmed moving video and successful
resize, while fresh receiver telemetry reported thousands of busy drops and only
one presented frame. Hardware decoding remained near 60 fps at 1920x1080.
That discrepancy was real evidence to investigate, not proof of frozen video.

The Qt frontend passes logical surface dimensions to FramePresentationBackend.
Its native implementation passed those hints to Nv12D3D11Presenter::Resize, while
Render independently corrected them using GetClientRect's native client pixels.
When those dimensions differ, the next frame repeats the resize. Resize redraws
the previous image, consuming the one-frame presentation queue before the new
frame's counted TryPresent call. Visible motion can therefore coexist with busy
new-frame drops, incorrect presentation counts and cleared input-frame mapping.

The native adapter now resolves the actual HWND client dimensions before Resize.
Frontend logical dimensions and input mapping remain unchanged. The one-frame
queue limit, nonblocking present policy, codec and congestion settings are preserved.

The generated-window regression deliberately supplies stale/logical hints of
17x13 for a larger real HWND, presents 90 paced frames and resizes halfway through.
It requires at least 30 counted new-frame presents and maximum frame latency 1.
It uses only a test-owned window, with no physical input, audio or screen capture.

- Before the fix: **3/90**, failed the unchanged test assertion.
- After the fix: **89/90**, passed; final rerun also **89/90**.
- Presentation worker/input safety tests pass (`VideoFrameInputTests`).
- The regression is included in the normal Windows CLI integration scenario and
  can run alone as `RoomCliWindowsTests --presentation-sizing-test`.

The laptop's original report is preserved at
`build/webrtc/laptop-first-tests/viewer-before-sizing-fix.json` and host telemetry
at `build/webrtc/laptop-live-window/host.stdout.log`. The updated viewer is installed
beside the original as `ScreenShareUi-sizing-fix.exe`, with its own source-diff and
binary identity manifest. The original package manifest is not relabeled.

The updated two-PC check is pending. Neither the local regression nor a user
confirmation of visible motion establishes physical latency, HDR/privacy, controller
acceptance or completion of Stages 2–4.
