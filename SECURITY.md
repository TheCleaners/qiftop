# Security policy

## Why this file matters

`qiftop-agent` runs as **root** with `CAP_NET_ADMIN`, owns a well-known
system-bus name (`org.qiftop.NetworkAgent1`), and exposes per-flow
network accounting (effectively the entire conntrack table) to local
DBus clients. The unprivileged GUI (`qiftop`) talks to it from the
user session, and a self-elevation fallback path (`pkexec` / `kdesu` /
`gksudo` / `lxqt-sudo` / `beesu` / `x-terminal+sudo`) exists for
machines where the agent isn't installed.

A bug in any of those surfaces — the DBus contract, the conntrack
parser, the privilege escalator's environment handling, or the
unauthenticated bits of the handoff socket — can be a local
privilege-escalation or information-disclosure vulnerability. We
take these seriously.

## Supported versions

`qiftop` is pre-1.0. Only the latest tagged release on `master`
receives security fixes. There are no LTS branches.

| Version | Supported          |
|---------|--------------------|
| 0.1.x   | :white_check_mark: |
| < 0.1   | :x:                |

## Reporting a vulnerability

**Please do not file a public GitHub issue for security bugs.**

Use GitHub's private vulnerability reporting on this repository:

1. Go to <https://github.com/TheCleaners/qiftop/security/advisories/new>.
2. Describe the issue with enough detail to reproduce: affected
   version, distro, agent capability tokens
   (`busctl --system get-property org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces Capabilities`), and a PoC if
   you have one.
3. We'll triage within **7 days** and aim to land a fix within
   **30 days** for high-severity issues, longer for lower-severity or
   architectural ones.

If GitHub's private reporting is unavailable, open a generic
"contact" issue asking for a private channel — do **not** include
vulnerability details there.

## In scope

- Local privilege escalation via the agent or the self-elevation
  fallback (`PrivilegeEscalator`, `HandoffServer`).
- Unauthenticated DBus method abuse from outside the `netdev`
  group, or amplification attacks from inside it (e.g. forcing the
  agent into permanent fast-poll cadence).
- Memory-safety bugs in the conntrack or netlink parsing paths
  reachable from a hostile peer or hostile kernel state.
- CSV / spreadsheet-injection bugs in the export path that could
  execute as formulas when the user opens an exported file.
- DBus signal payload parsing in the GUI that can be triggered by a
  malicious agent (e.g. via name-squatting on a session bus).

## Out of scope

- Local denial of service achievable only by users already in the
  `netdev` group (they can already see every flow on the host).
- Bugs in upstream dependencies (Qt, libnl, libnetfilter_conntrack,
  libdbus) — please report those upstream and CC us.
- Resource exhaustion via legitimate use of the GUI (e.g. opening
  many filters; we'll happily take performance patches, but it's
  not a security issue).
- Distros that ship the agent without the bundled DBus policy
  file (`dist/dbus/org.qiftop.NetworkAgent1.conf`) — that policy
  is what enforces the `netdev` gate.

## Security architecture (quick reference)

- The agent **does not** trust DBus peers. All method calls require
  `netdev` group membership (enforced by the system-bus policy file).
- The agent's idle manager treats peer-supplied cadence hints as
  ceilings only, clamped to `poll.min_interval_ms` (default 100 ms),
  with per-peer TTLs so a disconnected peer can't pin the agent at
  fast-poll forever.
- Connection snapshots are **capped** (`kMaxConnections = 4096`
  flows, top-N by bytes) so a pathological conntrack table can't
  blow up the wire payload or the GUI.
- `PrivilegeEscalator` forwards environment variables to the
  privileged child via an **allowlist** (not a denylist). Common
  loader knobs (`LD_PRELOAD`, `LD_LIBRARY_PATH`, `LD_AUDIT`,
  `QT_PLUGIN_PATH`, …) are dropped.
- The `HandoffServer` listening socket authenticates clients with a
  256-bit nonce delivered out-of-band via env var, with a strict
  pre-auth message size cap.
- Exported CSV fields beginning with `=`, `+`, `-`, `@`, `\t`, or
  `\r` are prefixed with a literal apostrophe so they can't be
  interpreted as spreadsheet formulas.

See [`AGENTS.md`](AGENTS.md) §4 (DBus contract) and §7 (coding
conventions) for the long-form rationale behind these choices.
