# Native clients

- `ui/`: Qt shell, pushed room directory and modular room/session pages.
- `cli/`: modular room command handling and executable entry point.
- `shared/`: room configuration, profile, launch parsing and diagnostics.
- `updater/`: standalone update executable.

UI and CLI use the modular backend by default. The obsolete legacy frontend
adapters and create/watch/share windows have been removed. Shared platform and
input implementations remain in `../backend`; transport and adaptation do not
belong in the frontend. Resources remain in repository-level `assets/`.

See [CUTOVER.md](../refactor/CUTOVER.md) for default service and upgrade behavior.
The visual redesign is the next stage; this cutover preserves the existing shell.
