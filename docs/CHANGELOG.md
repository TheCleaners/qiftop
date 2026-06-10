# Changelog

All notable user-facing changes to **qiftop** will be documented in this
file. The format follows [Keep a Changelog](https://keepachangelog.com)
and the project versioning follows [SemVer](https://semver.org).

Pre-release alpha tags (`v0.1-alphaN`) are intentionally omitted —
their commit history is preserved in `git log`. This file is the
human-readable summary for *shipping* releases.

## [0.2] — 2026-06-09

### Added

- **Per-flow process & container attribution** — every flow in the
  Connections view can now carry the owning PID + `comm` + UID, plus
  the container runtime / id / name where applicable. Driven by a
  pluggable resolver chain wired into the agent: `SockDiagResolver`
  walks `NETLINK_SOCK_DIAG` to map 5-tuples → socket inode → PID;
  `CgroupClassifier` parses `/proc/<pid>/cgroup` to classify the
  process into a container scope; `NetnsScanner` extends sock_diag
  coverage into every non-host network namespace so container-side
  flows attribute correctly (requires `CAP_SYS_ADMIN`). All bounded
  caches with `(pid, starttime)` PID-reuse guards.
- **Nested container chain** — for stacked runtimes (e.g. k3s pods
  inside docker-launched k3d nodes), qiftop walks the full OUTER →
  INNER ancestry and exposes it via `ConnectionDto.containerChain`.
  Tooltip shows the breakdown when chain depth ≥ 2; the chain badge
  ▸ in the Container column hints at the count. Pinned by 18
  real-world `/proc/<pid>/cgroup` fixtures + Tier-2 integration
  runners for docker, podman, k3d, naked k8s.
- **Process and Container columns** in the Connections view —
  capability-gated against the agent's wire tokens, hidden by
  default to preserve the v0.1-iftop-like baseline. Settings →
  Display → "Process & Container Attribution" controls visibility +
  chain-in-tooltip rendering. Header right-click also offers per-
  column toggles. `ConnectionAttributionDelegate` renders the
  PID/runtime/name + a small annotation in the same gauge-overlay
  style as the rest of the row.
- **On-demand `GetProcessDetails(pid)` RPC** — clients fetch
  `exe`/`cmdline`/`cwd`/`startTime` lazily on right-click rather
  than carrying them in every snapshot. Cache key for clients is
  `(pid, startTime)` so PID reuse never serves stale data.
- **Filter mini-language extensions** — `pid=`, `uid=`, `comm:`,
  `runtime=docker`, `container:<haystack>` (multi-search across
  runtime / id / name), and `chain_has:<runtime>` (matches any
  ancestor in `containerChain`). `pid=0` selects unattributed
  flows by design.
- **Grouped views in the Connections tab** — new View dropdown:
  Flat (default, v0.1 look), By Interface, By Container, By
  Process. Group rows aggregate rate / byte / packet sums and
  child-flow counts; per-group sort by aggregated value works
  via column-header click. Persisted across runs.
- **Row context-menu attribution actions** — Filter by Process /
  Container / Runtime, Copy process / container info. All filter
  values are quoted via the filter mini-language so colons,
  spaces, etc. don't break parsing.
- **RPM / Fedora packages** — `cpack -G RPM` now produces
  `qiftop` + `qiftop-agent` `.rpm`s alongside the existing `.deb`s,
  following RPM naming conventions
  (`qiftop-<ver>-1.fc<NN>.x86_64.rpm`). Library deps are discovered
  by rpm find-requires; `agent.conf` is `%config(noreplace)`; a
  native `%post`/`%postun` scriptlet pair manages the `netdev`
  group + systemd reload. Prerelease tags map `-rcN` → `~rcN` so rpm
  sorts the prerelease before the final, mirroring the `.deb`
  behaviour. The release workflow builds and publishes both package
  formats as assets.

### Changed

- **DBus contract bumped to Version `"0.5"`** (additive):
  - `ConnectionDto` extended with `pid`, `uid`, `comm`,
    `containerRuntime`, `containerId`, `containerName`,
    `containerChain[(runtime,id,name)]`.
  - New `Connections.GetProcessDetails(uint pid) → (...)` method.
  - Six new capability tokens: `process-attribution-wire`,
    `container-attribution-wire`, `container-chain-wire`,
    `process-attribution`, `container-attribution`,
    `container-chain`, `netns-scan`, `on-demand-process-details`.
  - The contract version 0.1 → 0.5 spanned pre-release alphas;
    only the 0.5 wire ships in v0.2. Older clients fail to
    unmarshal and fall back to the in-process backend via the
    existing probe.
- **`postinst` auto-enrols the installing user into `netdev`**
  via `$SUDO_USER` / `$PKEXEC_UID`. Users still need to log out
  or `newgrp netdev` for the membership to take effect, but the
  manual `usermod -a -G netdev "$USER"` step is no longer
  required for typical sudo / pkexec installs.
- Connections view is now a `QTreeView` (was `QTableView`) to
  host the grouped modes. Flat mode is a strict pass-through and
  remains pixel-identical to v0.1 (no indentation, no decoration,
  same delegates).

### Fixed

- Per-flow throughput gauge dark fill is painted on the Connections
  view again (regression introduced when the view migrated to
  `QTreeView`; `RowGaugeDelegate` was casting unconditionally to
  `QTableView*` and silently bailing).
- Connections-view header click now actually sorts rows
  (`ConnectionGroupProxy::sort()` was missing — Qt's default no-op
  was inherited).
- `/user.slice/.../user@<uid>.service` and its app-service
  descendants no longer mis-attribute as `systemd` units; on every
  desktop these would otherwise label every browser tab / Wayland
  helper as a container.
- `ConnectionGroupProxy` no longer reset-storms on every
  `dataChanged` / `rowsInserted` / `rowsRemoved` from the source;
  Flat mode is now a strict 1:1 pass-through that preserves
  selection and scroll.
- Container column on grouped rows no longer mis-renders as
  `(host)`; the delegate falls through to the model's aggregate
  text (`—`) for group rows.

### Tier-2 attribution integration

- Live `qiftop-attribution-probe` runners for **docker** and
  **rootful podman** ship under
  `tests/integration/attribution/runners/`, gated by
  `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON`. Docker + podman run on
  push-to-main / dispatch / release in CI. k3d and naked-k8s (k0s)
  runners exist for local Vagrant runs — cold bring-up is ~3-4 min
  each so they're not in CI; the chain shapes they cover are
  pinned by Tier-1 unit tests.

## [0.1] — 2026-06-08

The first qiftop stable release. A Qt 6 iftop-style Linux network
monitor shipped as two Debian packages (`qiftop` GUI client +
`qiftop-agent` privileged DBus daemon). See the
**Initial release highlights** subsection further down for the
comprehensive feature list; the entries directly below cover the
late-cycle polish landed between `v0.1-alpha2` and `v0.1`.

### Security

A pre-v0.1 security audit (sub-agent code review + manual pass) surfaced
seven findings worth addressing before tagging stable. Fixed in this
release:

- **HIGH — Privileged child no longer inherits the user's `PATH`.**
  `PATH` was on the env allowlist forwarded into the root child via
  `pkexec env PATH=… qiftop`. Any future `QProcess` / `findExecutable`
  in the privileged process would have resolved through user-controlled
  directories first — a latent LPE primitive. `PATH` is now dropped from
  the allowlist; both `sessionEnv()` (root child) and `scrubbedHelperEnv()`
  (helper subprocesses) force `PATH=/usr/sbin:/usr/bin:/sbin:/bin`.
- **MEDIUM — Handoff socket can no longer be slot-camped by a same-uid
  process.** `HandoffServer` now evicts an unauthenticated incumbent in
  favour of a newcomer (so an attacker holding the slot pre-HELLO can't
  lock out the real privileged child) and gates every accepted peer on
  `SO_PEERCRED` (must be the same uid as the parent, or root).
- **MEDIUM — Handoff nonce is no longer passed on argv.** Previously the
  256-bit nonce travelled via `pkexec env QIFTOP_HANDOFF_NONCE=hex …`,
  which is world-readable via `/proc/<pid>/cmdline` for the lifetime of
  the polkit prompt. The nonce is now written to a 0600 file under
  `$XDG_RUNTIME_DIR` (or `$HOME/.cache/qiftop/handoff-XXXXXX/`); only the
  file path is forwarded via `QIFTOP_HANDOFF_NONCE_FILE`. The child
  reads + unlinks immediately. The env-var form is still accepted for
  one release for transitional compat.
- **MEDIUM — `HandoffServer` no longer falls back to `/tmp`.** When
  `$XDG_RUNTIME_DIR` is empty/missing (headless / minimal sessions),
  the server now `mkdtemp`s a 0700 directory under
  `~/.cache/qiftop/handoff-XXXXXX/` instead. The previous `/tmp`
  fallback exposed a `bind()`-then-chmod permission-race window during
  which a different uid could connect.
- **MEDIUM — `agent::loadIdleConfig` no longer suffers signed-int
  overflow on huge schedule/timeout values.** Seconds are now bounded
  to `[0, 86400]` before being multiplied into milliseconds in 64-bit;
  out-of-range values fall back to the compile-time default with a
  warning (admin-only input, but UB is UB).
- **LOW — `IdleManager::setClientHint` now returns `bool` and the
  services only count *accepted* hints as activity.** Previously a peer
  rejected from the (capped) hint table could still pin the agent at
  the active cadence by hammering `SetDesiredIntervalMs`.
- **INFO — `HandoffServer` post-auth read buffer is now capped at 1 MiB.**
  Was unbounded; low real risk (peer is the privileged child) but a
  robustness gap.

Defence-in-depth items deferred to v0.2: per-helper PATH-injection
checks, full Desktop Entry escaping in `Autostart`, additional systemd
hardening knobs (`SystemCallFilter`, `ProtectProc=invisible`, etc.).

### Added
- **Help → Keyboard Shortcuts…** (bound to `F1`) dialog enumerating every
  shortcut and context-menu action, so the global `QShortcut` bindings
  (`Ctrl+F`, `Esc`, `Ctrl+C`, `Ctrl+1`…`Ctrl+9`) — which don't appear in
  any menu — are discoverable.
- **Per-table column widths / order persisted** across runs
  (`QHeaderView::saveState` for both Interfaces and Connections tabs).
  Window geometry, window state, current tab, and sort order were
  already persisted; column layout joins them.
- **`Ctrl+F`** focuses the filter expression bar (switches to the
  Connections tab first if needed) and selects its contents.
- **`Esc`** clears the filter expression when the filter bar has focus.
- **`Ctrl+C`** in either table copies the selected rows to the
  clipboard — connections use the existing flow-line formatter,
  interfaces fall back to tab-separated cells.
- **`Ctrl+1` / `Ctrl+2`** switch tabs (generalised to Ctrl+N for up
  to 9 tabs in case the count ever grows).
- **Connections context menu**: new "Show only flows to/from <peer>"
  and "Hide flows to/from <peer>" entries seed the filter expression
  with `host="<addr>"` (or `not host="<addr>"`). Quoted form is used
  so IPv6 colons don't confuse the parser.
- **Empty-state placeholder** on the Connections tab when no flows
  match — a centered, palette-aware rich-text hint suggesting
  `ping 1.1.1.1` and a Ctrl+F reminder. Replaces the previous
  blank-table-that-looks-broken UX.
- `QIFTOP_TESTS_SANITIZE` CMake option (`OFF` / `address` /
  `undefined` / `address+undefined` / `thread` / `leak`). Applies
  sanitizer flags per-test-target only, leaving production binaries
  untouched. Ships with `tests/lsan.supp` to filter Qt/glib/dbus
  one-shot statics. 16/16 tests pass under `address+undefined`.
- `SECURITY.md` documenting the vulnerability-reporting process. The
  agent runs as root with `CAP_NET_ADMIN`; a private channel for
  security reports is now linked from the README.
- `tests/test_units.cpp` — pins down IEC unit boundaries and decimal
  precision in `util::formatBytes` / `formatByteRate`. Catches future
  edits that accidentally switch to SI (1000-based) units.
- `tests/test_priv_escalator.cpp` — pins the env-var allowlist that
  gates which variables `PrivilegeEscalator` forwards into the root
  child. Adding `LD_PRELOAD` (or similar loader knobs) to the
  allowlist would now fail CI deliberately. Exposes new public statics
  `PrivilegeEscalator::envAllowlist()` and `filterEnv()`.

### Fixed
- README: corrected the test-count badge (was stale) and the
  filter-expression regex example (the `~` regex value needs quotes
  when the regex contains `.`).

### Initial release highlights

The first qiftop release ships two Debian packages:

* **`qiftop`** — unprivileged GUI client.
* **`qiftop-agent`** — privileged DBus daemon (DBus-activatable;
  optional systemd unit). Speaks `org.qiftop.NetworkAgent1` on the
  system bus.

#### Feature highlights

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

#### Architecture

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

#### DBus contract — `org.qiftop.NetworkAgent1`

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

#### Security

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

#### Packaging & install

* Two `.deb` packages built via `cpack -G DEB`: `qiftop` (GUI) and
  `qiftop-agent` (root daemon).
* Ships `dist/conf/agent.conf` as a Debian conffile (user edits
  survive package upgrades), plus the systemd unit, DBus
  activation file, system-bus policy, and desktop entry / icon.
* `RelWithDebInfo` + unstripped binaries for pre-release tags
  (debuggable crash reports from early testers); `Release` +
  stripped for the eventual stable tag.

#### Testing

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

#### Known limitations

* Linux only. The configure step on any other platform fails
  fast with `No backend available for platform: <name>` — by
  design until a BSD/macOS backend lands.
* No man pages yet (deferred to v0.2).
* No separated debug-info `.ddeb` companion packages yet — alpha
  builds carry full debug info inline; stable .deb is stripped.

[0.2]: https://github.com/TheCleaners/qiftop/releases/tag/v0.2
[0.1]: https://github.com/TheCleaners/qiftop/releases/tag/v0.1
