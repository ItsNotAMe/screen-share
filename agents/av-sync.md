# A/V Sync Notes

## Current WebRTC rooms

Default host system-loopback and microphone capture now follow Windows console
role routing notifications (render and capture flow respectively). Registration
precedes default resolution so activation races are detected. Callbacks only
advance an atomic revision; the existing producer worker retires the old native
endpoint, discards its pending/in-flight PCM, opens the new default and primes
with paced silence. Room/audio settings revisions remain unchanged. Explicit
device IDs stay pinned; process capture remains isolated. Mute/Stop cancels
the producer, including replacement activation. Failed replacement activation
uses the existing failure policy below, rather than a second retry mechanism.

Shared-source capture now retries failed native capture endpoints on its existing
producer worker, with 250 ms–5 s capped backoff. The selected resolver remains
authoritative on every read; no app-to-system fallback is allowed. A new target
bypasses old backoff, and in-flight PCM spanning a target change is silenced.
Capture-drop counts survive endpoint retirement. The outer switchable capture
keeps its 10 ms output clock/silence while activation waits; Mute/Stop retire and
cancel the old producer.

Explicit system/microphone capture now also recovers through SwitchablePcmCapture
on its existing producer boundary, with 250 ms–5 s capped backoff. It reuses the
committed factory/selection, preserving pinned device IDs and room/audio revision.
A replacement is healthy only after its first PCM arrives; a recovery producer
has a five-second first-PCM deadline. Factories, activation, processing and teardown
stay on producer owners. Microphone processing is recreated for every open.
Manual Retry/source selection/mute supersede automatic activation and backoff;
Close/Stop prevent further attempts. Old producer queues are discarded and drop
counts survive replacement. Shared-source mode keeps its inner validated-target
policy; explicit process IDs retain manual retry rather than following PID reuse.
Native calls that do not honor cancellation still cannot be preempted in-process.

Viewer output now follows default-device notifications (render/console role)
on the existing playout worker. Callbacks only advance an atomic revision;
registration is retained until unregister, and activation races are not lost.
Explicit devices do not follow defaults. Output failures use 250 ms exponential
backoff capped at five seconds, with manual updates taking priority and no
reopen after Close. Recovery preserves settings/revision, primes with silence
instead of the in-flight pre-activation block, and then resumes live pulls.
Paced discard continues during outages; do not add a replay queue or pause video.

Windows ownership/callback rules follow [stream-routing guidance](https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-routing-implementation-considerations)
and [IMMNotificationClient restrictions](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient).
Default route flow/role filtering follows [OnDefaultDeviceChanged](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-ondefaultdevicechanged).
The injected PCM/UI proofs do not qualify physical hot-plug or every driver.

The shipping room receiver uses WebRTC A/V synchronization. Audio NetEq now has
a ten-packet limit (200ms at the negotiated 20ms Opus cadence) and fast
acceleration. The former upstream 200-packet reserve could retain seconds of
audio after an output stall, leaving sound behind and allowing synchronization
to add video delay. Keep recovery inside NetEq: manually replaying missed pulls
in a burst also advances its tick-based jitter estimator and can inflate its
target delay. Device-free PCM pacing preserves the 10ms phase across ordinary
overshoot/work, skips missed slots and remains cancellable.

`av-sync-recovery` uses the production runtime, real H264/Opus loopback transport
and stalled synthetic output in Quality and Gaming. It checks received media
NTP timing after repeated 400/600/1200ms stalls and continued video delivery.
This reproduces a recovery failure, not the original cause of a remote machine's
stall. Report PCM callback gaps, capture drops and audio/video jitter-buffer
target/minimum delays distinguish scheduling trouble from packet jitter.

## Historical UDP receiver

Default receiver behavior auto-enables A/V sync for `--preview --audio-playback`. `--no-av-sync` keeps raw timing for diagnostics. Explicit `--av-sync` remains accepted.

Key idea: audio and video are already multiplexed through the same UDP sender/socket. Sync problems mostly come from receiver-side pipeline latency and independent playout clocks, not separate network paths.

Important implementation points:

- H.264 sender QPC is stamped around stream encoder output timing, not desktop capture timing.
- Decoder input samples carry the packet's sender QPC directly so late joins after a GOP boundary do not assign stale timestamps to decoded frames.
- Synced audio waits for the preview playout clock and schedules packets against the same sender-QPC timeline.
- Synced audio playback latency should be at least the preview latency.
- Automatic and explicit sync fall back to video-only after 30 video frames if audio never arrives, or after 750 ms if previously active audio stops. This prevents host mute, device loss, and silence/no-packet capture behavior from wedging preview playout.
- In video-only fallback, do not use stale or late audio renderer timestamps to gate preview or audio rendering. The fallback must stay video-first so silence/no-audio periods cannot freeze the share.
- When fresh audio returns, discard audio that predates the currently presented video timestamp before restoring normal synchronized playout.
- Only accepted, monotonically advancing media timestamps refresh diagnostic clocks. Late UDP packets must not regress or revive a stale clock.
- Viewer status uses actual playout skew only while audio, video, and playout clocks are all ready. It reports `No audio`, `Waiting`, or `Syncing...` during transitions instead of exposing a stale raw timeline delta.
- Final `Done` stats can be misleading after the sender stops because video stops before queued audio drains. Judge periodic live lines first.

Current receiver catch-up direction:

- If the preview catches up and audio falls behind, prefer dropping queued, not-yet-rendered audio packets so the session stays real-time.
- Do not try to correct large lag by holding video as the primary behavior. Video should remain the live timeline.
- A preview safety ceiling is still useful because audio already submitted to WASAPI cannot be removed; it prevents a visible jump hundreds of milliseconds ahead of audible audio while queued audio is being trimmed.
- Avoid reciprocal gating deadlocks: if the preview is currently held by the audio safety ceiling, the audio renderer must bypass the last-presented-video gate for that cycle so queued audio can advance and unblock preview playout.
- Track `av_sync_audio_catchup_drops`, `av_sync_audio_gate_bypasses`, and `preview_sync_waits` in reports to tell whether audio catch-up, audio gate bypass, or the video safety ceiling is doing work.

Useful test command shape:

```powershell
.\build\release\ScreenShare.exe --udp-recv 5000 --preview --audio-playback --log receiver.log
.\build\release\ScreenShare.exe --display 0 --seconds 18000 --udp-send HOST:5000 --adapt-bitrate --adapt-resolution --audio-capture system --log sender.log
```
