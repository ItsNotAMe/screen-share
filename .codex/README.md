# Repository Workflow Notes

This directory points local tooling at the repo memory files. Keep durable project notes in
`agents/`; keep generated reports, logs, and one-off scratch files out of git.

When resuming this repo, read:

1. `agents/project-memory.md` for current state, preferences, and recent merges.
2. `agents/todo.md` for the ordered roadmap and backlog.
3. Topic files as needed:
   - `agents/repo-map.md`
   - `agents/live-pipeline.md`
   - `agents/av-sync.md`
   - `agents/adaptation.md`
   - `agents/packaging.md`
   - `agents/ui.md`
   - `agents/lan-discovery.md`
   - `agents/nat-traversal.md`
   - `agents/security.md`

Workflow reminders:

- Use local `git` and `gh` for GitHub work. Do not use the GitHub connector for this project.
- Keep PRs small and self-review before opening or merging.
- Avoid public branch, commit, PR, or tracked-file text that depends on one contributor's local tooling.
- After every PR merge, tell the user the next recommended step.
