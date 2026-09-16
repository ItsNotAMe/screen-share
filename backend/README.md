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

`room/qt/RoomNetwork` owns the dedicated room-network event loop, admission and
bounded socket command/event queues. `media/RoomPeerRoster` maps authenticated
snapshot generations/revisions to peer lifecycle hooks. These are used by the
real-room headless scenario; normal UI/CLI facade adoption remains in progress.

`room/qt/RoomSessionCoordinator` automatically dispatches network events and
asynchronous send completions on signaling; `RoomSignalCodec` provides structural
wire conversion after transport authentication. Both live in the shared
ScreenShareRoomSession target. Consumers should not add a manual event pump.

`api/RoomSession.h` is the asynchronous v2 room-session owner. It owns network and
signaling executors, admission, authenticated routing, bounded reconnect policy,
thread-safe status and an asynchronous media-drain barrier. A RoomRuntimeFactory
constructs native media on signaling; no Qt/WebRTC types cross this control API.
NativeRoomRuntime composes shared capture, peers, recovery/retirement and initial
stream preferences. WindowsRoomRuntimeFactory binds WGC/WASAPI and selects the
capture device before codec creation. The public-session proofs inject synthetic
endpoints or use a generated WGC window through this same production runtime.
ScreenShareMediaAdapters is shared by application/proof builds. UI/CLI selection,
live settings and presentation wiring remain pending; this API does not silently
switch the legacy ScreenShareSession facade.
