# Native backend

Reusable session API, media, capture, audio, room transport and Windows services
live here. UI/CLI code lives in `../frontend`; the separately deployed room service
lives in `../signaling-worker`.

Backend targets expose this directory as their include root, never the frontend
directory. Includes such as `media/HostMediaSession.h` remain stable for consumers.
Qt Core/Network/WebSockets are backend dependencies for rooms; Qt Widgets is not.

`api/` is the current application-facing facade. `media/` and `room/` implement
the v2 replacement. `runtime/`, `transport/` and parts of `core/` still support the
legacy default until the refactor acceptance gates permit cutover and removal.
The CLI still uses runtime diagnostic entrypoints. The legacy Win32 receiver
window remains beside its D3D presenter in `render/` pending presentation adoption;
this directory split does not claim those architectural migrations are complete.

WebRTC native headers stay in `media/webrtc/`. Public commands must enqueue work
and publish results without blocking the UI. Capture, signaling and room sockets
retain their explicit thread/lifetime ownership. See `../refactor/PLAN.md`.
