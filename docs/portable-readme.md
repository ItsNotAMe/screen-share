# ScreenShare portable

Extract this entire folder, then open **ScreenShareUi.exe**. Keep the DLLs and
plugin folders beside the executables. No backend flag is needed.

Create a room on the host, then join its room ID or new room link on the viewer.
Both PCs must use this modular client. Old v1 rooms and direct UDP invites are
not compatible; create a fresh room after upgrading both endpoints.

This release uses the isolated v2 service with a ten-room cap.
An alternate compatible service can be selected with `--signal-server HTTPS_ORIGIN`.

Command-line examples, from this folder:

```powershell
.\ScreenShare.exe --create-room --name "My room" --display 0
.\ScreenShare.exe --join-room ROOM_ID
.\ScreenShare.exe --join-room ROOM_ID --decoder software
.\ScreenShare.exe --help
```

Use software decoding if hardware decoding causes problems. Remote input requires
host permission, granted directly or after a request; Ctrl+Alt+Shift+F12 revokes input. The portable
package does not install a controller driver. Multi-controller allocation remains
under investigation; the application must not take over an occupied local slot.

Use **Save diagnostic report** in the room page when reporting an issue.
Close the application before replacing its files. The separate updater retains
its signed-package checks; no server migration or public release is performed by
running this build. ScreenShare uses Qt under LGPLv3. See
`THIRD-PARTY-NOTICES.md` and `licenses/` for license texts, corresponding Qt
source downloads and instructions for replacing the dynamically linked libraries.
