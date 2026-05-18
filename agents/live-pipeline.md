# Live Pipeline Notes

## Sender

Capture runs at the requested FPS. WGC is the default backend; DXGI Desktop Duplication is fallback/comparison. Capture can reuse the latest desktop frame when the desktop has not updated, so `output_fps` can stay near 60 while `desktop_update_fps` is lower.

For live UDP:

1. Capture produces BGRA/scRGB plus GPU NV12 texture when stream encoding is active.
2. `H264StreamEncoder` uses software by default for the `--share` preset; raw stream encoding still defaults to `--stream-encoder auto`.
3. Encoded H.264 access units are fragmented into UDP datagrams by `UdpSender`.
4. The UDP sender paces datagrams by current video bitrate with modest headroom plus audio payload bitrate.
5. Receiver feedback updates bitrate/resolution policy when `--adapt-bitrate` / `--adapt-resolution` are enabled.

`--share` is the normal live preset and should not allow a many-second sender queue to build. It
defaults to a 1500 ms UDP queue cap unless the user explicitly overrides `--udp-max-queue-ms`.
Raw `--udp-send` keeps the uncapped diagnostic default.

Important sender stats:

- `stream_queue`: hardware encoder async input queue depth.
- `stream_dropped`: frames dropped by the hardware encoder input queue before encoding.
- `udp_queue_ms`: future send time currently queued in the paced UDP sender.
- `udp_pending`: datagrams still waiting in the sender queue.
- `udp_peak_queue_ms`: useful for spotting stale live send backlog in reports.
- `udp_pacing_bitrate_mbps`: actual pacer target, intentionally above encoder bitrate to cover packet overhead and short bursts.
- `bitrate_advice_reason`: why adaptation wants to hold/reduce/increase.

## Receiver

Receiver reassembles UDP fragments, orders H.264 access units, decodes with Media Foundation, and queues preview frames by sender/video timestamps. Audio packets are reassembled, decoded from Opus when needed, and scheduled against the preview playout clock when A/V sync is active.

If Watch stays open while Share stops and starts again, the receiver treats a video frame-id rewind with a newer sender QPC clock as a fresh stream. On that restart it clears pending receiver media queues, restarts H.264 decode/playout, clears audio playout/decoder state, and resets A/V sync diagnostics. It also ignores any late frames whose sender QPC belongs to the previous stream. This is general receiver lifecycle behavior, not a NAT-only path.

Important receiver stats:

- `h264_decode_resyncs` and `h264_decode_skipped_packets`: damaged stream recovery.
- `preview_late_drops`, `preview_overflow_drops`, `preview_sync_drops`: visible preview playout drops.
- `audio_playback_latency_drops`, `audio_playback_sync_drops`, `audio_playback_sync_waits`: audio trim/sync behavior.
- `av_playout_audio_ahead_ms`: best live sync indicator while sender is still running.
- `stream_restarts` / final `receiver stream restarts`: receiver-side detection of a fresh sender run while Watch remained open.
- `stream_stale_frames` / final `receiver stale stream frames`: late old-stream frames ignored after a restart.
