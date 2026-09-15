# Native clients

- `ui/`: Qt Widgets desktop UI and its session-event adapter.
- `cli/`: command parsing, diagnostic dispatch and executable entrypoint.
- `updater/`: standalone update executable.

Clients consume the shared backend API and build targets. Backend code must not
include client headers. UI resources continue to use repository-level `assets/`.
The QtSessionBackend name describes the UI adapter; the session implementation
it invokes belongs to `../backend`.

The folder move preserves behavior. Normal UI/CLI sessions still use the legacy
runtime until v2 integration and acceptance are complete. Put new transport,
adaptation, recovery and session ownership in the backend, not in client adapters.
