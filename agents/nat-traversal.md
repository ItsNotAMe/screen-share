# NAT Traversal Notes

## Direction

- Free-first path:
  - Tailscale/manual IP is already usable through the Share address field.
  - Tailscale peer picking is merged as a separate UI path through optional `tailscale status --json` integration when the CLI is installed. Do not expect UDP LAN broadcast discovery to cross Tailscale.
  - Current focus is STUN plus manual invite exchange for direct UDP hole punching.
  - Hosted rendezvous/relay is a far-back fallback only if direct hole punching plus Tailscale/manual IP are not enough. Do not prioritize it while direct hole punching is viable.
- Keep LAN discovery separate from Internet traversal. LAN discovery is broadcast-based and does not cross most routers or mesh VPN boundaries.

## Current State

- `main` now contains the direct manual NAT building blocks:
  - `--stun HOST[:PORT]` prints the public/local UDP endpoint for a temporary socket.
  - `--make-invite PORT --stun HOST[:PORT]` queries STUN from the chosen local UDP port and prints a compact invite. `ss1e:` invites are encrypted with the shared access code and hide endpoint/session metadata; `ss1p:` invites are compact plaintext for explicit `--allow-plaintext` mode. Legacy `nat_invite=screenshare-invite-v1;...` blobs still parse for compatibility.
  - `--make-invite` also prints `watch_command_template`, `share_command_template`, and `probe_command_template` lines. These intentionally use `<PEER_INVITE>` and `CODE` placeholders so the raw access code is not echoed.
  - `--nat-probe PORT --peer-invite INVITE` binds the chosen local UDP port and sends small probe/reply packets to the peer invite's public and local endpoints.
  - `--udp-local-port PORT` lets Share/live UDP send bind a chosen local sender port for manual media experiments after a successful probe.
  - `--local-invite INVITE` on Share binds the UDP sender to this side's invite local port, which is the safer live-flow equivalent of manually typing `--udp-local-port`.
- PR #72 merged: Watch/UDP receive accepts `--peer-invite INVITE` and sends NAT punch probes from the real receive socket while waiting for media.
- The Share feedback socket should ignore NAT probe datagrams instead of counting them as invalid feedback; otherwise probes can make reports look noisy or unhealthy.
- PR #73 merged: Share can take a receiver invite directly as `--share "nat_invite=..."`, validate the invite security, and send media to the invite public endpoint.
- PR #74 merged: `--invite-endpoint auto|public|local` works with `--share "nat_invite=..."`. Auto currently uses public; local is for same-LAN/VPN/manual experiments until probe-result endpoint selection exists.
- PR #75 merged: Share auto mode retargets its UDP sender to the source endpoint of incoming Watch NAT probe datagrams, with access-code fingerprint checks when encrypted.
- Both peers must currently exchange invite lines manually, but the Qt UI can now create/copy this side's invite so users no longer need the CLI just to produce the blob.
- Probe mode is diagnostic only. The live Watch path sends NAT punch probes from the real receive socket, and live Share can bind from `--local-invite` plus retarget from those probes.
- PR #77 merged: NAT status diagnostics add `nat_status` / `nat_hint` fields to sender and receiver logs so reports are easier to read without manually combining probe, retarget, and feedback counters.
- PR #78 merged: invite output is more guided with copy/paste command templates.
- PR #79 merged: UI bridge for manual invites. Share can paste the receiver invite plus optional local invite; Watch can paste the sender invite. This is not the final room UI.
- PR #80 merged: UI create/copy buttons for each side's local NAT invite.
- PR #81 merged: guided UI polish for invite exchange status, clipboard paste/extract, and Share-side guardrails.
- PR #83 merged: UI live NAT status display from engine `nat_status` / `nat_hint` output.
- PR #85 merged the full branch to `main`.
- PR #88 testing showed the one-sharer-invite UI path works locally/reachable-direct, but not through the tested NAT: Watch sent public/local probes while Share saw `udp_nat_probe_packets=0` and stayed `waiting_for_probe`. This is the expected endpoint-filtered NAT deadlock for a one-link, no-server flow, not a decoder/media bug.
- Current UI supports the no-server two-sided fallback: Watch can create a My invite response from its listen port, and Share can paste that Friend invite while still passing its own room invite as `--local-invite`.
- CLI multi-viewer NAT groundwork: extra viewers can be added with `--share-target PEER_INVITE --share-target-local-invite LOCAL_INVITE`. Each extra local invite must be generated from a unique sender port so that Watch probes the same sender socket that will send media. The UI exposes repeatable watcher rows; per-viewer status still needs room work.

## Follow-Up Shape

- Live Watch probe source retargeting plus Share `--local-invite` binding is the first automatic endpoint selection path; still needed: richer setup summaries and UI guidance.
- Next multi-viewer UI slice should make watcher rows statusful: each row already owns a sharer local invite and pasted watcher response invite, but it still needs live status.
- Next diagnostic layer should keep failures readable: no probes seen, probes rejected, retargeted but no feedback, receiver probing, receiver media rejected, and receiver actively receiving.
- For generic Internet NAT without an external rendezvous/relay, one side must still learn the other side's endpoint somehow. Practical no-server options are the UI two-sided response invite, Tailscale/manual IP, or router port mapping/UPnP. Compact encrypted invites make that exchange shorter and less leaky, but do not remove the need for endpoint exchange.
- User already validated the UI-guided direct invite flow after create/copy invite landed. Since later changes only added receiver restart recovery and status display, treat the core invite mechanics as validated.
- README now carries the two-computer UI checklist for future NAT/Tailscale validation.
- Still needed: richer live probe/media status only if reports still show confusion.
- Keep rendezvous/relay out of the current workstream unless direct mode proves insufficient for the target users.
