# Third-party software in ScreenShare 1.0.0

ScreenShare uses **Qt 6.10.3** (Qt Core, Gui, Network, Widgets, Svg and
WebSockets, plus their runtime plugins), copyright The Qt Company Ltd. and
other contributors, under the GNU Lesser General Public License version 3.
Qt's bundled third-party components retain their individual licenses.
License and copyright texts are included in `licenses/qt`; Qt's supplied
software bills of materials are included in `licenses/qt-sbom`.

The unmodified corresponding Qt module sources are available alongside the
installer and portable ZIP at:
https://github.com/ItsNotAMe/screen-share/releases/tag/v1.0.0

- qtbase-everywhere-src-6.10.3.tar.xz
- qtsvg-everywhere-src-6.10.3.tar.xz
- qtwebsockets-everywhere-src-6.10.3.tar.xz

Archive SHA-256 hashes are recorded in `licenses/qt/SOURCE-SHA256.txt`.
Original archives: https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/

Qt is dynamically linked. You may modify or replace these libraries, and
reverse engineer ScreenShare to debug modifications to them. To run a modified
compatible Qt build, close ScreenShare, extract the portable ZIP to a writable
folder and replace the Qt DLLs and plugin DLLs with your Windows x64 MSVC
build. Keep the same runtime ABI and directory layout. No application signature
check prevents loading your replacement libraries. ScreenShare source and
build instructions are also available in the public repository at tag v1.0.0.

**WebRTC and its bundled dependencies** are statically linked from the pinned
build recorded in `cmake/dependencies`. Their generated license/copyright
notices are included in `licenses/webrtc`, including bundled codec libraries.
Build scripts, patches and pinned source revisions are in the repository.

**ViGEmClient** and the optional installer-provisioned **ViGEmBus** retain their
MIT and BSD licenses; see `ViGEmClient-LICENSE.txt` and `ViGEm-NOTICE.txt`.
**ICU 72.1** is covered by `ICU-LICENSE.txt`, copied from the upstream
`unicode-org/icu` release-72-1 source tree.
**Microsoft Visual C++ runtime** DLLs are Microsoft's redistributable runtime
components and remain subject to Microsoft's license terms.

ScreenShare itself is licensed under Apache-2.0; see `LICENSE`.
