# Security policy

## Why this file matters

`qiftop-agent` is the privileged boundary. It runs as **root**, owns the
system-bus name `org.qiftop.NetworkAgent1`, and uses the capabilities needed
for capture and attribution: `CAP_NET_ADMIN`, `CAP_SYS_PTRACE`,
`CAP_DAC_READ_SEARCH`, and `CAP_SYS_ADMIN`. The shipped systemd unit further
restricts it with a capability bound, `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome=yes`, `PrivateTmp=yes`, restricted address families, and
`RestrictNamespaces=net`.

`qiftop` and `nqiftop` are unprivileged clients. They prefer the DBus agent and
only use in-process capture as a fallback. The agent exposes host-wide counters,
conntrack flows, process/container attribution, and
`GetProcessDetails(pid)`, so bugs in access control, parsing, disclosure,
elevation, handoff IPC, or export sanitisation can be security issues.

## Supported versions

`qiftop` is pre-1.0. Only the latest tagged release receives security fixes;
there are no LTS branches.

| Version            | Supported          |
|--------------------|--------------------|
| latest release     | :white_check_mark: |
| older releases     | :x:                |

## Reporting a vulnerability

**Please do not file a public GitHub issue for security bugs.**

Use GitHub private vulnerability reporting:

1. Go to <https://github.com/TheCleaners/qiftop/security/advisories/new>.
2. Include the affected version, distro, client (`qiftop` or `nqiftop`), agent
   capability tokens, and a PoC if available. To collect tokens:

   ```bash
   busctl --system get-property org.qiftop.NetworkAgent1 \
     /org/qiftop/NetworkAgent1/Interfaces \
     org.qiftop.NetworkAgent1.Interfaces Capabilities
   ```

3. We will triage within **7 days** and aim to fix high-severity issues within
   **30 days**. Lower-severity or architectural issues may take longer.

If private reporting is unavailable, open a generic "contact" issue asking for
a private channel. Do **not** include vulnerability details there.

## In scope

- Local privilege escalation via `qiftop-agent` or GUI self-elevation
  (`PrivilegeEscalator`, `HandoffServer` / `HandoffClient`).
- Bypassing the DBus `netdev` gate for method calls or signal subscription.
- Cross-UID disclosure through `GetProcessDetails`, especially `exe`, `cwd`, or
  `cmdline`.
- Bypassing cadence clamps, hint caps, hint expiry, or snapshot bounds to pin
  unsafe root-agent work.
- Memory-safety bugs in conntrack, netlink, sock_diag, cgroup, or DBus DTO
  parsing reachable from hostile kernel state or a malicious peer.
- CSV / spreadsheet-injection bugs in exported data.
- Client DBus payload parsing bugs triggerable by a malicious or name-squatting
  agent, especially on a development session bus.

## Out of scope

- Generic local denial of service by users already authorized through `netdev`,
  unless it bypasses an explicit safety bound above.
- Bugs in upstream dependencies (Qt, libnl, libnetfilter_conntrack, libdbus,
  ncurses). Please report those upstream and CC us.
- Resource exhaustion from normal interactive client use, such as opening many
  filters. Performance patches are welcome, but this is not normally security.
- Distros that omit the bundled DBus policy file
  (`dist/dbus/org.qiftop.NetworkAgent1.conf`), which enforces the `netdev` gate.

## Security architecture (quick reference)

- **Privilege split:** the root agent is the only component that talks to the
  kernel capture APIs, cross-UID `/proc`, or container network namespaces.
- **DBus access control:** the system-bus policy denies agent calls/signals by
  default, then allows root and `netdev` members.
- **Least-disclosure process details:** `GetProcessDetails(pid)` returns
  low-sensitivity fields to any authorized caller; by default `exe`, `cwd`, and
  `cmdline` go only to root or the target process owner.
- **Configurable detail policy:** `/etc/qiftop/agent.conf` supports
  `[process_details] disclosure=owner|permissive|restricted` plus optional
  `allow_users` / `allow_groups` for restricted cross-UID admin access.
- **Cadence and payload bounds:** cadence hints are clamped, capped, expire by
  monotonic TTL, and connection snapshots are capped at 4096 top-talkers.
- **Elevation and handoff hardening:** elevated children receive an audited
  environment allowlist and safe fixed `PATH`; handoff uses a per-user socket,
  peer credentials, a 0600 nonce file, and capped read buffers.
- **CSV export sanitisation:** fields starting with formula-trigger characters
  are prefixed with an apostrophe before export.

See [`AGENTS.md`](AGENTS.md) §4 (DBus contract), §7 (coding conventions), and
§8a (process/container attribution lifetime rules) for the detailed rationale.
