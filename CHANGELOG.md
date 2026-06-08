# Changelog

All notable user-facing changes to **qiftop** will be documented in this
file. The format follows [Keep a Changelog](https://keepachangelog.com)
and the project versioning follows [SemVer](https://semver.org).

Pre-release alpha tags (`v0.1-alphaN`) are intentionally omitted —
their commit history is preserved in `git log`. This file is the
human-readable summary for *shipping* releases.

## [Unreleased]

## [0.1] — first public release

The first qiftop release. A Qt 6 iftop-style Linux network monitor
shipped as two Debian packages:

* **`qiftop`** — unprivileged GUI client.
* **`qiftop-agent`** — privileged DBus daemon (DBus-activatable;
  optional systemd unit). Speaks `org.qiftop.NetworkAgent1` on the
  system bus.

### Highlights

* **Real-time per-interface throughput** — Qt model/view tab with
  hand-drawn row gauges and tray-icon sparklines. Counters come
  straight from libnl-3 / rtnetlink, refreshed every second by
  default and on demand via the agent's adaptive idle scheduler.
* **Per-connection flow table** — every active conntrack flow with
  byte / packet rates, direction (inbound / outbound / forwarded),
  per-flow interface attribution (works for forwarded flows too),
  TCP state, and an iftop-style mini-language filter expression
  bar.
* **First-class IPv4 + IPv6 throughout** — including a workaround
  for the universally-buggy `nfct_query(AF_UNSPEC)` (we issue
  separate AF_INET and AF_INET6 dumps and ship a small inert
  `inet qiftop` nftables shim to ensure conntrack tracking for both
  families).
* **System-bus DBus agent** with built-in cadence hints, idle
  wind-down, snapshot truncation, monotonic snapshot timestamps,
  and a Version + Capabilities probe surface so libqiftop-style
  third-party consumers can gate on feature presence without
  parsing introspection XML.
* **DNS resolution is always client-side** with in-process LRU
  cache; the agent never sees hostnames.
* **System tray** with live throughput sparkline; optional autostart
  to tray on login (XDG `~/.config/autostart/qiftop.desktop`).

### Architecture

* Two-binary design: `qiftop` GUI talks to `qiftop-agent` over DBus
  (system bus in production, session bus in dev with `--session`).
* GUI falls back automatically to an in-process backend (`pkexec`
  / `sudo` self-elevation + a nonce-authenticated handoff socket)
  when the agent isn't installed or the user isn't a member of the
  `netdev` group.
* Layered, platform-portable code: `backend/<platform>/` isolates
  all kernel-touching code; `ui/`, `dbus/`, `util/`, `config/`,
  `dns/` are all platform-agnostic. New backends slot in by
  inheriting `NetworkMonitor` + `ConnectionMonitor`.
* Long-term direction (deliberately *not* in v0.1, but the contract
  shape and layering already accommodate it): a `libqiftop`
  shared library extracting the DTOs / aggregation / filter
  mini-language for future consumers (Prometheus exporter,
  alerting daemon, ncurses TUI). See AGENTS.md §2 "Future
  direction".

### DBus contract — `org.qiftop.NetworkAgent1`

* **Version**: `"0.3"` (additive; breaking changes will branch
  `NetworkAgent2`).
* **Wire format**: native Qt marshalling, but designed so non-Qt
  consumers can decode — `proto` is the IANA L4 number (RFC 5237),
  protocols / TCP states / interface oper states are documented in
  AGENTS.md §4 alongside the byte-exact tuple signatures.
* **Per-snapshot CLOCK_MONOTONIC timestamp** on both data signals
  so downstream rate computation isn't disturbed by DBus delivery
  jitter or wall-clock jumps.
* **Adaptive polling** — the agent honours `SetDesiredIntervalMs`
  hints from each connected client, takes their `min()`, and winds
  down to paused after configurable idle thresholds. Effective
  cadence is broadcast back via `CadenceChanged`.
* **Bounded payloads** — connection snapshots are capped at 4096
  top-talkers-by-bytes; the agent logs a `qWarning` when it
  truncates so a busy router never produces an unbounded DBus
  message.
* **12 capability tokens** advertise optional behaviour;
  feature-detect by token presence, not Version comparison. See
  AGENTS.md §4 for the full table.

### Security

* The system-bus policy restricts every method call and signal
  delivery to members of the `netdev` group — closes both a
  netlink-amplification DoS (any local user pinning the root
  agent at 100 ms forever) and an information-disclosure path
  (`GetConnections` returning every flow on the host to any
  unprivileged caller, including other users' ephemeral ports).
* `postinst` ensures `netdev` exists and auto-enrols the installing
  user (`$SUDO_USER` / `$PKEXEC_UID`); users need to log out / `newgrp
  netdev` for the membership to take effect.
* The privilege escalator uses an env-var **allowlist** (not denylist)
  before invoking `pkexec` / `sudo` / `kdesu` / `gksudo` / `lxqt-sudo` /
  `beesu`, so future loader knobs (`LD_AUDIT`, `QT_PLUGIN_PATH`, …)
  can't sneak in through gaps in a hand-curated blocklist.
* The "Relaunch as administrator" IPC handoff socket is
  nonce-authenticated with a 256-bit secret + 1-second pre-auth
  message-size cap, so a co-resident unprivileged process can't
  hijack it.
* CSV export sanitises spreadsheet-formula injection (leading
  `=`, `+`, `-`, `@`, `\t`, `\r`) so attacker-controlled hostnames
  (via reverse DNS) or kernel-supplied interface names can't
  execute when the user opens the exported file.

### Packaging & install

* Two `.deb` packages built via `cpack -G DEB`: `qiftop` (GUI) and
  `qiftop-agent` (root daemon).
* Ships `dist/conf/agent.conf` as a Debian conffile (user edits
  survive package upgrades), plus the systemd unit, DBus
  activation file, system-bus policy, and desktop entry / icon.
* `RelWithDebInfo` + unstripped binaries for pre-release tags
  (debuggable crash reports from early testers); `Release` +
  stripped for the eventual stable tag.

### Testing

* 14-test ctest suite running under `dbus-run-session` on every
  CI build (Ubuntu 22.04 + 24.04 × Debug + Release):
  pure-logic unit tests (heuristics, EMA, filter mini-language,
  exporter, settings migration, autostart), agent-internal tests
  (`IdleManager`, `Config`), DBus type round-trip, DNS cache,
  handoff socket auth, proxy model behaviour, **and** a black-box
  integration test that spawns a real `qiftop-agent --session`
  and exercises Version / Capabilities / GetInterfaces /
  SetDesiredIntervalMs end-to-end over a temporary bus.
* CI also runs a packaging-QA gate: `lintian --fail-on error`,
  `desktop-file-validate`, and a Docker-based smoke install on
  a fresh Ubuntu 24.04 image.

### Known limitations

* Linux only. The configure step on any other platform fails
  fast with `No backend available for platform: <name>` — by
  design until a BSD/macOS backend lands.
* No man pages yet (deferred to v0.2).
* No separated debug-info `.ddeb` companion packages yet — alpha
  builds carry full debug info inline; stable .deb is stripped.

[Unreleased]: https://github.com/TheCleaners/qiftop/compare/v0.1...HEAD
[0.1]: https://github.com/TheCleaners/qiftop/releases/tag/v0.1
