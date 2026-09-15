# Media Foundation / WebRTC integration

These are private integration headers, not portable session APIs. The independent
`tools/webrtc-proof` project builds them against the pinned artifact. Normal UI/CLI
sessions have not switched to this path.

`MfVideoEncoderFactory` exposes H.264 High level 4.2, packetization mode 1,
up to 1920×1080 / 60 FPS. Supplying an `MfHardwareSession` enables hardware
probing with the session's D3D device; omitting it selects software. Each
encoder owns one MTA worker and one replaceable pending raw frame; only one frame
is submitted to the transform at a time. Replaced/suspended frames
generate WebRTC dropped-frame callbacks. No encoded frame queue is introduced.
Negotiation of a lower level is explicitly rejected until per-level encoder
limits are implemented; it must not silently encode above the negotiated level.

WebRTC API calls must remain serialized, as required by its codec contract.
Initialization, rate changes, callback registration and release use worker
barriers. Encoding returns after replacing the pending slot. A generation rejects
old posted jobs after reset. Release cancels pending input and waits for the active
callback and COM cleanup before returning. Callbacks must not synchronously call
back into codec lifecycle operations.

SetRates applies the assigned bitrate directly. Zero suspends even when its FPS
field is also zero. FPS changes restart the transform and force a keyframe;
bitrate-only updates do not restart it. Software input must yield exactly one
encoded packet. Software errors or a completed encode call exceeding 500 ms mark
the encoder failed. Hardware uses explicit readiness/submission and nonblocking
MF event polling, with a 500 ms deadline and generation cancellation. The old
hardware queue and multi-second drain loops are not used. Polling waits at most
one millisecond between checks on the encoder worker, never on signaling/network
threads. The MF event batch is bounded to 64.

Before reporting hardware acceleration, initialization probes two assigned rates
and forced keyframes at the configured dimensions. Hardware failure marks the
session/device implementation quarantined and restarts that viewer's encoder in
software at the same dimensions/rate, re-encoding the current retained frame as
a keyframe. Existing healthy encoders continue; newly initialized encoders do not
retry quarantined hardware. Cancellation does not quarantine the device. The
`PollOutput` boundary supports deterministic missing-output injection in tests.

The deadline handles missing asynchronous output, **not** a driver call that
never returns. COM initialization, ProcessInput/Output, shutdown and D3D Map are
not safely preemptible in-process. Device-removal recovery, multi-vendor checks
and broader soak/performance evidence remain unfinished.

`D3dVideoDevice` owns application immediate-context operations on one thread.
`D3dVideoFrameBuffer` retains an independently allocated NV12 texture and its
device owner. Published textures are never overwritten; the encoder holds them
through output/cancellation. Same-device hardware submission does not read back
pixels. Software or cross-device fallback maps on the owner, converts to I420
and caches the result under a mutex. Readback count/time are exposed for evidence.
MF's internal device-manager operations use D3D multithread protection. D3D COM
references can be released on another thread after the owner is joined.

GPU producers support synthetic upload and completed owned capture snapshots.
`CaptureConfig::ownedNv12` copies one reusable conversion target into a unique
published texture and checks GPU completion before publication. Polling uses a
1 ms interval and 50 ms failure deadline (a safety bound, not a latency target).
This cannot preempt a stuck driver call. The extra GPU copy means this is not
end-to-end zero-copy. `D3dVideoDevice` can retain the capture device;
`RetainCapture` rejects borrowed, incomplete, wrong-device or wrong-shaped frames.
Capture protects complete shader-state sequences when MF/readback share the
immediate context. Owned frames retain the device after capture stops.
WGC resize discards the old surface before pool recreation; frames, pool and
session are explicitly closed. Selected-window closure reports an error without
display fallback. Device recovery remains pending.

Owned capture reports Stopped/Active/Minimized/Closed on its capture-owner thread.
WGC closure callbacks capture a shared atomic flag, never the capture object;
the revoker is detached before teardown. Original process/thread identity is
checked for window sources, and a closed state remains closed until explicit
Start. The WGC item still binds the original source; no automatic HWND-based
reselection or display fallback occurs. Minimized windows drain queued frames
and publish none, with a short bounded wait to avoid busy polling. Closure and
minimization are checked again before publishing a completed GPU snapshot.
Remote status/placeholder UI remains session-integration work.

At capture polling and GPU completion boundaries, device removal is reported as
`CaptureDeviceLostError` with the HRESULT retained. Resource recreation and full
device-loss classification/recovery remain pending and unverified under real
device removal. The process-exit proof launches only its own generated source
child in a kill-on-close job, terminates it, checks permanent closure and stops
capture, repeated three times per process. Minimize/restore and replacement
window checks are part of the ordinary optional live-capture lifecycle test.
Device upload/readback calls are for capture/codec threads, not signaling/network
or UI threads. The harness uses a separate synthetic capture thread.

`MfVideoDecoderFactory` exposes High and constrained-baseline H.264. COM resource
creation/use/destruction stay on its own MTA worker; output callbacks run on
WebRTC's decode caller. It returns move-owned NV12 CPU buffers after MF visible-aperture
cropping. It does not advertise hardware acceleration. Encoded access units are
limited to 16 MiB, timestamp associations to 32, and coded dimensions to 4096².
Actual visible output is checked against WebRTC's configured maximum. MF may
enumerate a default larger output type before signaling the stream's real type,
so the allocation bound is distinct from the negotiated visible-frame bound.

Unique MF sample IDs associate delayed output with the original RTP/NTP values,
including RTP wraparound. Errors clear associations, reset the transform and
require a keyframe. Release discards delayed output; it never emits callbacks
from a retired stream. The short High-profile lifecycle test observes one delayed
decoded frame per cycle, discarded on release. This must be included in future
pipeline latency measurements.

Tests cover local MF→WebRTC RTP/SRTP→MF video plus three encrypted data channels,
zero-rate/resume, keyframes, 100-input burst replacement, reset/release, callback
thread ownership, timestamp wrap and 1080 cropping. `-Hardware` additionally tests
real GPU-backed PeerConnections, hardware burst/rate/lifecycle behavior,
missing-output fallback/quarantine, cancellation and cached concurrent readback.
They do not establish live capture/GPU presentation, cross-machine latency, or Gate A.

`-LiveCapture` adds a generated-window WGC test needing an interactive desktop
and capture-service access. It encodes 80 inputs through the hardware WebRTC
encoder adapter, resizes the source at fixed 640×360 output, closes the window,
and compares retained pixels after later writes and stop. Validation readbacks
are counted separately from encoder readbacks. The test runs three cycles in one
process. Queued frames are returned and GPU work flushed before closing the
session, then the pool; this sequence passes the reproduced shutdown regression.
Before COM uninitialization, the C++/WinRT factory cache is cleared to prevent
restart through a factory whose DLL has unloaded. The native clang-cl capture
translation unit enables CMPXCHG16B for the SDK's cache-clearing interlocked call.
This lifetime issue is documented by [Microsoft](https://devblogs.microsoft.com/oldnewthing/20211105-00/?p=105878).
The second live test sends WGC frames through hardware H.264 PeerConnections with
Opus and data channels, requiring 60 decoded frames and zero encoder readbacks.
`LiveCaptureSource` owns WGC on its capture worker, and stops/joins before normal
proof completion. External source-process termination, device
recovery and long lifecycle soaks remain unverified. Do not claim general shutdown
safety or preemption of hung OS/driver calls from these finite runs.
Query semantics follow [Microsoft's D3D query contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nn-d3d11-id3d11query);
resize handling follows [WGC guidance](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture).

## Receive presentation

`OwnedNv12Buffer` retains cropped decoder output without an eager I420 conversion.
Its I420 fallback is generated once under a mutex and shared by later callers.
This is still software decoding with CPU memory; it is not GPU decoding or a
zero-copy receive path. The existing even-dimension NV12 contract is preserved.

`LatestVideoFrameSink` retains one replaceable frame under a mutex. `Take` moves
the latest frame out; rendering/conversion never runs in the decode callback.
Stop clears the pending frame and permanently rejects later callbacks. Allocate
a new sink per session generation; remove it from the WebRTC track before
destroying it. The window owner consumes the pending frame at its render cadence.

`Nv12VideoPresenter` creates, uses and destroys D3D resources on the window's UI
thread. It uploads owned packed NV12 directly into the existing GPU shader path;
other layouts are repacked and I420 fallback is counted explicitly. Fit preserves
aspect ratio with bars. Rotation and odd/oversized dimensions are rejected.
The opt-in low-latency mode sets/reports a one-frame DXGI device queue limit and
uses nonblocking Present. Busy/occluded submissions are dropped without replay;
counts mean accepted DXGI submissions, not measured physical display updates.
WARP fallback is exposed; the live proof requires hardware acceleration. D3D
upload, resize and external driver calls can still block, so this is not a hard
end-to-end latency bound. Device-loss recovery is not implemented here yet.

The live PeerConnection proof also renders the receiver on its own window thread,
resizes it, verifies decoded neutral chroma and samples only its client-area
center/bars from the composed desktop. Compositor/display color transforms may
change RGB; the visible check verifies brighter content and dark letterboxing,
not calibrated color. The generated receiver window must remain unobscured.
Headless tests cover a 500-frame burst, stopped-sink rejection and concurrent
cached conversion. There is no audio prerequisite in the sink/presenter.
Queue configuration follows [Microsoft's DXGI guidance](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-setmaximumframelatency)
and [nonblocking presentation flags](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).

## PCM audio adapter

`PcmAudioDeviceModule` exchanges 48 kHz, 10 ms PCM with WebRTC; WebRTC owns
Opus and transport. Portable endpoint contracts live in `media/audio`.
Each endpoint is created, used and destroyed on its own worker. ADM control
methods require serialized calls; stop joins workers and callback registration
is a barrier against in-flight callbacks. A blocked driver call is not preempted.
The pinned `AudioTransportImpl` accepts frame counts but returns interleaved
sample counts from `NeedMorePlayData`; stereo output therefore returns 960.

WASAPI capture reuses explicit source/device selection and opts into PCM16 stereo.
Application capture buffering is capped at 30 ms with oldest-sample drops;
discontinuities clear partial blocks. OS capture buffering is separate. Mono
conversion averages stereo; general multichannel policy remains pending.
Playout has one 10 ms handoff, event-driven writes, software mute/volume and
actual buffer capacity/engine period diagnostics. It attempts the minimum shared
engine period, then reactivates the same selected device with shared conversion
when unsupported. Only EVENTCALLBACK is passed to the low-period initializer,
as specified by [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream).
Unknown engine periods are reported as zero, not inferred from buffer capacity.
Capture delay is an estimate, not a measured capture-to-speaker latency.

Device failures stop that direction and increment diagnostics; they do not
silently select another device. Microphone processing and device recovery policy
remain session-integration work. System/process proof tracks disable AEC/AGC/NS.
Tests cover synthetic Opus with video, mono/stereo, capture mute, repeated stop,
callback teardown and invalid device selection. `-AudioDevice` additionally
plays a quiet tone and captures only the test process, tests physical lifecycle,
and sends process-loopback audio through Opus alongside H.264. The received audio
is measured without physical replay to avoid feedback. Physical playout is
tested separately; other capture devices/modes and full A/V timing remain unverified.

Receiver presentation recovery uses `PresentationRecovery.h`: typed DXGI errors, owner-thread resource release, 250 ms drop-only backoff and at most three rebuilds per presenter lifetime. A fourth error is terminal. No failed frame is retained. Initial attachment and explicit Clear failures still propagate; session startup/shutdown must handle them. Capture/encoder recovery remains pending. Policy tests run by default; `PresentationRecoveryTest --gpu` requires a desktop session and tests injected loss with real resource recreation, not actual driver removal.

Device recovery boundary: capture must call MfHardwareSession::RetireDevice before DesktopCapturer::RebuildWindowDevice and before publishing new-device frames. The rebuild retains the original WGC item/closure subscription, supports only owned window capture, and stops on reconstruction failure. Encoders permanently reject old-device native frames without readback and request an IDR from fresh software input. Device retirement is shared across viewers and also detected via GetDeviceRemovedReason. This is an explicit primitive, not automatic session recovery. A coordinator must own replacement-device generations, invalidate all affected sessions, bound retries and suppress old-generation callbacks. Real device removal remains untested.

Automatic recovery proof: CaptureRecovery provides three rebuilds with 250 ms nonblocking backoff and terminal exhaustion. LiveCaptureSource catches typed device loss, retires its current owner, rebuilds the original WGC source, then publishes with a fresh owner/generation. D3dVideoDevice retirement rejects cached CPU pixels too, and encoder retirement checks now apply across replacement generations. Production callback-generation barriers are still pending. Run CaptureRecoveryTest --live in a desktop session; it injects loss without resetting the physical driver.
