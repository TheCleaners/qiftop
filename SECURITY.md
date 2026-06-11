# Security policy

## Why this file matters

`qiftop-agent` is the privileged boundary. It runs as **root**, owns the
system-bus name `org.qiftop.NetworkAgent1`, and is installed with the
capability set it needs for capture and attribution:
`CAP_NET_ADMIN`, `CAP_SYS_PTRACE`, `CAP_DAC_READ_SEARCH`, and
`CAP_SYS_ADMIN`. The shipped systemd unit also applies a sandbox
(`CapabilityBoundingSet` / `AmbientCapabilities`, `NoNewPrivileges`,
`ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, restricted
address families, and `RestrictNamespaces=net`).

The GUI (`qiftop`) and terminal frontend (`nqiftop`) are unprivileged
clients. They prefer the root agent over DBus and fall back to in-process
capture only when the agent is unavailable (the GUI can relaunch itself
through a self-elevation handoff path; the TUI must already have the
needed privileges for the fallback).

The agent exposes host-wide interface counters, the conntrack flow table,
process/container attribution (`pid`, `uid`, `comm`, container metadata),
and an on-demand `GetProcessDetails(pid)` RPC. Bugs in those surfaces —
DBus access control, kernel-data parsing, process-detail disclosure, the
privilege escalator, the handoff socket, or export sanitisation — can be
local privilege-escalation or information-disclosure vulnerabilities.

## Supported versions

`qiftop` is pre-1.0. Only the latest tagged release receives security
fixes; there are no LTS branches. The current project release is **0.2.2**.

| Version | Supported          |
|---------|--------------------|
| 0.2.2   | :white_check_mark: |
| < 0.2.2 | :x:                |

## Reporting a vulnerability

**Please do not file a public GitHub issue for security bugs.**

Use GitHub's private vulnerability reporting on this repository:

1. Go to <https://github.com/TheCleaners/qiftop/security/advisories/new>.
2. Include enough detail to reproduce: affected version, distro, whether
   you used `qiftop` or `nqiftop`, agent capability tokens
   (`busctl --system get-property org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces Capabilities`), and a PoC if you
   have one.
3. We'll triage within **7 days** and aim to land a fix within **30 days**
   for high-severity issues, longer for lower-severity or architectural ones.

If GitHub's private reporting is unavailable, open a generic "contact"
issue asking for a private channel — do **not** include vulnerability
details there.

## In scope

- Local privilege escalation via `qiftop-agent` or the GUI self-elevation
  fallback (`PrivilegeEscalator`, `HandoffServer` / `HandoffClient`).
- Bypassing the DBus system-bus policy from outside the `netdev` group,
  including unauthorized method calls or signal subscription.
- Cross-UID disclosure through `GetProcessDetails`, especially leaking
  `exe`, `cwd`, or `cmdline` to callers that should not see them.
- Amplification bugs that let a caller bypass cadence clamps, hint caps,
  or hint expiry and pin the root agent at an unsafe polling rate.
- Memory-safety bugs in conntrack, netlink, sock_diag, cgroup, or DBus DTO
  parsing reachable from hostile kernel state or a malicious peer.
- CSV / spreadsheet-injection bugs in the export path that could execute
  formulas when a user opens exported data.
- DBus signal payload parsing in clients that can be triggered by a
  malicious or name-squatting agent (especially on a development session bus).

## Out of scope

- Generic local denial of service by users already authorized for the agent
  via `netdev`, unless it bypasses an explicit safety bound above.
- Bugs in upstream dependencies (Qt, libnl, libnetfilter_conntrack,
  libdbus, ncurses) — please report those upstream and CC us.
- Resource exhaustion via normal interactive use of the clients (for
  example, opening many filters). Performance patches are welcome, but this
  is not normally a security issue.
- Distros that ship the agent without the bundled DBus policy file
  (`dist/dbus/org.qiftop.NetworkAgent1.conf`). That file is what enforces
  the `netdev` gate.

## Security architecture (quick reference)

- **Privilege split:** clients are unprivileged; the root agent is the only
  process that talks to libnl, nf_conntrack, sock_diag, cross-UID `/proc`,
  or container network namespaces.
- **DBus access control:** the system-bus policy default-denies both method
  calls (`send_destination`) and signal receipt (`receive_sender`) for
  `org.qiftop.NetworkAgent1`, then re-allows root and members of `netdev`.
- **Least-disclosure process details:** `GetProcessDetails(pid)` returns
  low-sensitivity fields (`pid`, `uid`, `comm`, `startTimeJiffies`) to any
  authorized caller. The privileged fields (`exe`, `cwd`, `cmdline`) are
  disclosed only to root or the target process owner by default.
- **Configurable detail policy:** `/etc/qiftop/agent.conf` supports
  `[process_details] disclosure=owner|permissive|restricted`. `owner` is the
  default; `permissive` exposes privileged fields to any authorized caller;
  `restricted` additionally allows configured `allow_users` / `allow_groups`
  for cross-UID administrator access.
- **Cadence hardening:** cadence hints are clamped to `poll.min_interval_ms`,
  capped per client table, counted as activity only when accepted, and expire
  by monotonic-clock TTL so a disconnected peer cannot pin fast polling.
- **Bounded snapshots:** connection snapshots are capped at 4096 flows,
  keeping the top talkers by total bytes.
- **PrivilegeEscalator environment allowlist:** elevated children receive
  only audited session variables plus a safe fixed `PATH`; loader/plugin knobs
  such as `LD_PRELOAD`, `LD_LIBRARY_PATH`, `LD_AUDIT`, and `QT_PLUGIN_PATH`
  are dropped.
- **Handoff IPC hardening:** the GUI handoff socket is created in a per-user
  runtime/cache directory, not `/tmp`; peers must pass `SO_PEERCRED`; the
  256-bit nonce is stored in a 0600 file whose path is forwarded via
  `QIFTOP_HANDOFF_NONCE_FILE`; and pre-/post-auth read buffers are capped.
- **CSV export sanitisation:** exported CSV fields beginning with `=`, `+`,
  `-`, `@`, tab, or carriage return are prefixed with a literal apostrophe so
  spreadsheet applications treat them as text.

See [`AGENTS.md`](AGENTS.md) §4 (DBus contract), §7 (coding conventions),
and §8a (process/container attribution lifetime rules) for the long-form
rationale behind these choices.
