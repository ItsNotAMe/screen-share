# A/V Sync Notes

Default receiver behavior auto-enables A/V sync for `--preview --audio-playback`. `--no-av-sync` keeps raw timing for diagnostics. Explicit `--av-sync` remains accepted.

Key idea: audio and video are already multiplexed through the same UDP sender/socket. Sync problems mostly come from receiver-side pipeline latency and independent playout clocks, not separate network paths.

Important implementation points:

- H.264 sender QPC is stamped around stream encoder output timing, not desktop capture timing.
- Decoder input samples carry the packet's sender QPC directly so late joins after a GOP boundary do not assign stale timestamps to decoded frames.
- Synced audio waits for the preview playout clock and schedules packets against the same sender-QPC timeline.
- Synced audio playback latency should be at least the preview latency.
- Automatic `--watch` sync falls back to video-only after 30 video frames if no audio packets arrive. This prevents muted/no-device audio failures from wedging preview playout with `av_sync_correction=waiting`.
- In video-only fallback, do not use stale or late audio renderer timestamps to gate preview or audio rendering. The fallback must stay video-first so silence/no-audio periods cannot freeze the share.
- Explicit `--av-sync` stays strict and does not take the video-only fallback.
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
