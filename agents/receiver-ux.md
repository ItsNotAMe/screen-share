# Receiver UX Notes

## Preview Window Controls

- `ReceiverPreviewWindow` owns the native Win32/D3D11 preview window.
- The first UX slice lives in `src/render/ReceiverPreviewWindow.*`.
- F11 and Alt+Enter toggle borderless fullscreen on the nearest monitor.
- Esc exits fullscreen without closing the receiver.
- The preview swap chain disables DXGI's default Alt+Enter handling with `DXGI_MWA_NO_ALT_ENTER`; otherwise DXGI can race the app's borderless fullscreen restore and leave the window chrome/title bar missing.
- F toggles between fit-to-window and 1:1 source-size scaling.
- 1 switches to 1:1 and resizes the window toward the current decoded source size, clamped to the monitor work area.
- Fit mode can upscale to fill the client area. 1:1 mode never upscales; it centers the source and scales down only when the window is too small.
- M toggles receiver audio mute when playback is enabled.
- + and - adjust receiver playback volume by 5% steps.

## Audio Playback Controls

- Receiver muted playback is intended for same-machine testing where WASAPI loopback would otherwise capture the receiver output again.
- `--audio-playback-muted` still receives, decodes, buffers, schedules, and renders audio packets; the WASAPI renderer submits silent buffers so timing and A/V sync keep running.
- `--audio-playback-volume PERCENT` sets the initial playback gain from 0 to 200 percent.
- Console telemetry includes `audio_playback_muted` and `audio_playback_volume_percent`.

## Receiver Logging

- Receiver stats suppress repeated full telemetry while no new UDP media is arriving.
- Idle receiver output prints one `waiting_for_stream` line with session, fingerprint, whether a stream was seen before, and current packet/frame counts.
- Full detailed receiver stats resume as soon as new video/audio datagrams arrive.

## Follow-Up Ideas

- If title telemetry becomes too dense, add a lightweight overlay instead of adding more title fields.
- Start real app UI with Qt after this receiver UX slice, keeping the current D3D preview separate at first.
