# Audio processing and format contract

The audio implementation batch completes microphone-only processing and explicit
multichannel downmix in the shared Windows runtime. Physical endpoint format
negotiation, unplug/replug and driver-specific acceptance remain open; synthetic
tests do not close those gates.

## Capture-local microphone processing

`AudioSwitchControl` accepts a microphone processor factory. WindowsRoomRuntime
supplies `ProcessMicrophone`; each microphone endpoint owns a fresh WebRTC APM on
its capture worker. It processes one interleaved PCM16 block of 480 stereo frames
at 48 kHz per read. There is no extra application queue, thread, service request,
renegotiation or per-viewer processing. One host capture supplies all viewers.

The policy enables the high-pass filter, moderate noise suppression and adaptive
digital gain (AGC2). It never changes the OS microphone gain. Echo cancellation is
disabled: this one-way host does not supply a synchronized speaker-reference
stream, so the implementation does not claim echo removal. APM may process speech
internally at another rate; its input/output contract remains 48 kHz stereo.

System, process and None selections never instantiate the speech processor. The
WebRTC shared audio track continues to disable global AGC/NS/AEC. Switching away
from a microphone destroys its processor with the endpoint on the capture worker.
Restart/retry creates one new processor, not nested wrappers or reused filter
history. No combined game/system signal is passed through microphone processing.

Candidate processing/start/read errors use the existing failed-handover behavior:
the previous healthy source remains selected. A live endpoint/processing failure
releases that endpoint and supplies paced silence while video continues. Explicit
same-source retry reconstructs capture and processing. None bypasses even the
capture factory and still opens no device. Existing 30 ms application buffering,
cancellation and one-command bounds remain in force.

`AudioSelectionStatus.microphoneProcessing` reports the configured policy. It is
independent of endpoint health: a failed selected microphone is still configured
for speech processing on retry. CLI exports this boolean, and the running-mic UI
explains the filtering and unavailable echo cancellation. System/process UI states
that speech processing is bypassed. No new device IDs or audio samples enter logs.

## Explicit stereo conversion

The v2 WASAPI capture adapter requests PCM16 at 48 kHz while preserving the selected
endpoint's native channel count and speaker mask. Windows handles sample-format
conversion and resampling; `StereoDownmix` owns the multichannel mixing policy.
Legacy callers retain their prior format-selection behavior. Process loopback's
virtual endpoint retains its existing PCM48 stereo contract; its already-stereo
output passes unchanged through the converter.

- Mono duplicates to left/right; ordinary stereo is bit-exact.
- Standard speaker-mask layouts up to eight channels are accepted, including 5.1
  back, 5.1 side and 7.1. Front-left/right remain on their corresponding sides;
  center and back-center feed both; surrounds/front-of-center feed their side at
  a -3 dB coefficient. LFE is omitted from the stereo fold-down.
- Fixed per-output normalization reserves full-scale headroom for simultaneous
  channels. This can reduce surround-source volume relative to native stereo; it
  avoids clipping and level pumping. No dynamic gain is applied to system audio.
- Unknown surround masks, inconsistent counts, unsupported positions and malformed
  packet sizes fail explicitly rather than guessing a channel order or another
  device. Mono/stereo without a mask have unambiguous defaults.
- The converter allocates no per-frame storage. Capture keeps at most 20 ms of
  converted stereo plus the existing one-block handoff. Overflow drops old frames;
  discontinuities clear pending PCM, and silent packets remain exact zeroes.

## Validation and acceptance

`PcmAudioDeviceTest` uses in-memory PCM and synthetic/discarded endpoints. It checks
mono/stereo identity, each channel of 5.1/7.1 independently, numeric fold-down gains,
full positive/negative scale, silence and rejected layouts/packet sizes. Actual
WebRTC APM suppresses a synthetic DC input while an unprocessed endpoint retains
it. Tests check cancellation, endpoint ownership, processor-start failure rollback,
fresh processors across retry/restart, and bypass for system/process/None.

The production UI/CLI scenarios exercise live mic selection through real Opus
connections. UI tests lose/retry microphone capture while video progresses, then
switch to process and system audio. CLI timed changes expose processing and its
removal. Four-viewer recovery, mute/output changes and video continuity remain in
the regression suite. All these tests are silent and use no physical input.

Remaining acceptance: real mono/stereo/surround endpoint negotiation (including
44.1 kHz float devices), physical microphone quality and gain behavior, explicit
device changes/unplug/replug, actual buffering/latency and driver hangs. Do not use
these tests as external gaming latency or hardware audio-quality evidence.
