# Per-viewer GPU source scaling

The Windows source adapter now fits and letterboxes retained NV12 textures on the
GPU before delivering them to WebRTC. Fixed canvases retain their requested size;
Auto uses WebRTC's pixel/FPS restrictions and never upscales beyond the source.
Matching dimensions pass through without allocating another texture.

## Ownership and scheduling

`D3dNv12Scaler` samples the luma and interleaved chroma planes directly into a new
NV12 texture. Linear sampling preserves the existing SDR YUV representation; there
is no RGB conversion or CPU staging map in this path. Black bars use limited-range
Y=16, U=128, V=128. Source image dimensions and centered offsets use the same even
alignment as the CPU fallback. Different sampling filters need not give identical
pixels on detailed/downsampled content; subjective text quality remains to assess.

One scaler belongs to each capture D3D device. Shaders/sampler are created lazily
once and released on its existing owner thread. Complete drawing sequences hold
the device's multithread lock, shared with capture and MF. Output textures are new
owned resources, never overwritten or pooled while another viewer retains them.
Commands are flushed before publication; completion does not block the CPU.

`GpuSubmissionQueue` bounds already-submitted scaling work to **four conversions
per device**, in addition to existing latest-frame handoffs. Event queries are
polled without flushing or waiting. A full queue drops the incoming frame and
increments `gpuBusyDrops`; it does not trigger CPU readback or quarantine. No
additional worker, timer or media queue was added.

Unsupported plane views, shader creation or other GPU scaling failures disable
scaling on that device for the remainder of its lifetime. Future frames use the
existing counted I420 fallback; a replacement device starts with fresh capability
state. Device retirement forbids scaling/readback of old textures. A software
encoder can still read back a successfully scaled texture later: source GPU scaling
is not a claim of hardware encoding or end-to-end zero-copy video.

## Status and input geometry

The public per-peer `source` snapshot includes cumulative scaled/GPU/fallback/
busy/drop counters, the last accepted canvas/image rectangle, scaling path, and
observed settings revision. A dropped frame never acknowledges a new revision.
`gpuReadbackFallbacks` counts scaling fallback operations; cached readback can be
shared, so it is not a physical GPU-map count.

UI details and CLI peer JSON expose `source.scalingPath` (`unknown`, `unchanged`,
`gpu`, `cpu`, `cpu-readback`) and `source.activeImage` (`left`, `top`, `width`,
`height`, or null before observation). These describe the last adapted source
frame, not current frame progress or receiver presentation. Per-frame cumulative
counters remain in the backend API; they do not force UI/CLI refreshes on every
frame. No additional service or WebRTC stats requests are introduced.

The rectangle is available for the later input implementation. Receiver-side
generation binding, consent and coordinate mapping are still required before it
can authorize or map input.

## Silent validation

`StreamSettingsTest.exe --gpu` covers GPU down/upscaling, letterboxing, YUV colors
and orientation, retained output, concurrent viewer sizes, Auto pixel restriction,
invalid geometry, real unsupported-resource fallback, quarantine and retirement.
It asserts zero readbacks during successful scaling before explicit test pixel
readbacks. The portable submission test holds all four completions pending across
100 admission probes and verifies bounded capacity and recovery on completion.

`MfHardwareAdapterTest.exe` feeds an actual GPU-scaled texture to hardware H.264,
checks that it avoids readback, then exercises existing software fallback and
retired-device recovery. `WindowsRoomSessionProof` uses generated WGC capture,
four real peers and decoded media; all four must report GPU scaling without scaling
readback fallback after a live resolution change. It uses synthetic audio and no
physical mouse/keyboard input. These checks do not require a preview swap chain.

Measured latency, physical GPU hangs, broader GPU compatibility, hardware decoder/
presentation work and the prior DXGI preview occlusion failure remain separate gates.
