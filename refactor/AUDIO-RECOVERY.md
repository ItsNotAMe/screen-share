# Audio endpoint failure and explicit recovery

Capture and playback wrappers now isolate **reported** endpoint startup and live
I/O failures from the media session. They release the failed endpoint on its owning
worker and keep ADM callbacks running: zero PCM for failed host capture, paced
discard for failed viewer output. Video, signaling and other viewers continue.
The configured device/source remains selected; no other physical device is opened
as an automatic fallback.

## State and retry contract

`AudioSelectionStatus.health` and `PlaybackStatus.health` expose `state` and a
monotonic `failures` counter:

- `inactive`: endpoint worker is not active or is stopping.
- `running`: the configured physical/test endpoint has started. Mute can still be on.
- `silent`: host deliberately selected None, which opens no capture device.
- `failed`: the selected endpoint failed and the wrapper is supplying silence or
  discarding output until an explicit command. This is not intentional None.

Failures of an active/starting endpoint increment the counter. Failed replacement
attempts return the existing typed command error and preserve the current selection,
revision and health; they do not invent additional failure/recovery episodes.
The counter survives endpoint stop/restart within its control lifetime. Low-level
ADM worker-error counters are distinct from these handled endpoint failures.

Call `SwitchAudioSource` or `UpdatePlayback` again to retry, including with the
**same** device ID/source. Capture commits on the candidate's first PCM block;
playback commits on its first accepted write. A successful retry increments only
the corresponding audio/playback revision. Failed retry preserves silence/discard;
failed replacement of a healthy endpoint preserves the old working endpoint.
There is one pending command, no reconnect, renegotiation, service request,
background re-enumeration or automatic retry loop. Starting an endpoint worker
again after all viewers leave is still a new startup attempt.

`PcmBlockPacer` provides cancellation-aware 10 ms fallback pacing without catch-up
bursts. Discard retains no replay queue and reports zero device buffering. Endpoint
creation, use and destruction retain their existing worker ownership. Stop cancels
pending changes and publishes inactive state; late failures cannot revive it.

## Frontends

The session window has separate health feedback, leaving command results visible.
Failure changes the existing action to **Retry selected audio** or **Retry playback**.
Users can retry the current selection or choose another endpoint. None still shows
intentional capture disablement. Existing volume/mute state is retained on retry.

CLI status adds `audioHealth` and `playbackHealth`, each with `state` and `failures`.
Device IDs and native exception strings are not added to status. An endpoint outage
alone does not end the CLI session. A subsequent timed `audioChanges`/
`playbackChanges` command can retry; ordinary explicit-command failure policy is
unchanged. Each timed entry remains a full selection, not a partial update.

## Validation and limits

Silent PCM tests inject startup failure, live read/write loss, failed retry and
same-device recovery. They verify endpoint release/thread ownership, no automatic
factory retries, silence/discard, preserved gain and shutdown. Actual offscreen UI
tests start a host with failed capture, retry through its widgets, then lose and
retry viewer output while decoded video continues. CLI tests report failed output
and recover through a timed same-device command. Four-viewer coverage injects host
capture loss, observes decoded silence with continued video for all viewers, and
checks isolation/recovery of one viewer's failed output.

These tests use synthetic PCM only, never physical audio playback. Actual hardware
unplug/driver-specific recovery remains open. A synchronous native call that never
returns cannot be forcibly preempted; this implementation does not claim to solve
driver hangs. Microphone-only processing and explicit multichannel conversion are
now implemented; see AUDIO-PROCESSING.md for their policy and acceptance limits. Receiver jitter
buffers can still drain audio already transmitted before capture failure.
