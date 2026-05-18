# Adaptation Notes

Adaptive bitrate and resolution are still opt-in. Normal live testing should use both together:

```powershell
--adapt-bitrate --adapt-resolution
```

The latest LAN logs showed:

- Resolution tiers: `2560x1440@1`, `1920x1088@0.75`, `1280x720@0.5`.
- `1920x1088` is expected: 1080 rounds to a 16-pixel H.264-friendly height.
- The app reduced to 1280x720 because bitrate hit its adaptive floor and sender queue pressure persisted.
- Freezes remained even at 1280x720 with receiver `preview_late_drops=0`, `preview_overflow_drops=0`, and `h264_decode_resyncs=0`.
- Sender showed multi-second `udp_queue_ms` plus growing `stream_dropped`, so the freeze source is sender-side stale queue / hardware encoder input backlog, not receiver recovery.

Local tuning now:

- Queue pressure uses queued time (`udp_queue_ms >= 750`) before reducing, rather than tiny datagram counts.
- When resolution scaling is enabled, the default adaptive bitrate floor is lower than target/4 so bitrate can fall further before resolution drops.
- Resolution reduction requires several pressure-at-floor reports.
- Hardware encoder async input queue is shorter, so stale frames are dropped earlier.

Current sender queue fix branch:

- User's 2026-05-15 release sender report showed native 2560x1440 stayed at `resolution_scale=1` while bitrate reduced from ~35 Mbps to ~4.75 Mbps.
- `udp_peak_queue_ms` reached about 7.5 seconds and `stream_dropped` reached 76, so the visible "slow" behavior was stale queued send time, not capture/encode cost.
- `--share` now defaults to a 1500 ms UDP live queue cap unless the user explicitly sets `--udp-max-queue-ms`.
- Adaptive resolution can step down when sender queue pressure stays around 1.2s or worse and bitrate has already been reduced to 60% or less of the original target, instead of waiting only for the absolute bitrate floor.
- Follow-up report from 05:00 showed the app reached 1280x720 and still had `stream_queue=10`, rising `stream_dropped`, and `udp_queue_ms` near 0.8-1.2s at the bitrate floor.
- New suspicion/fix: the UDP pacer was too exact. It paced datagrams at roughly encoder bitrate + audio bitrate, which ignores H.264 bursts, encryption/protocol overhead, and catch-up drain. The branch now applies 125% video pacing headroom before adding audio payload bitrate.
- Short local tests showed the NVIDIA hardware MFT path dropped queued inputs even at 640x360 startup, while software had zero `stream_dropped` at 640x360, 1280x720/60, and native 2560x1440/60. The `--share` preset now defaults to software unless the user passes `--stream-encoder hardware`.

## Backlog Link

The remaining freeze/stale-frame work lives in `agents/todo.md` so there is only one task list.
