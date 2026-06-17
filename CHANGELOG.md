# Changelog

All notable changes to qiftop are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Agent attribution knobs** — `/etc/qiftop/agent.conf` now has an
  `[attribution]` section for startup-only resolver tuning:
  `eagerness=off|balanced|eager`, per-layer `process` / `container` /
  `netns` switches, and guarded cache/netns refresh overrides. No wire change;
  disabled layers simply stop advertising their existing capability tokens.
- **Performance benchmark harness** (developer tooling) — opt-in `bench/`
  microbenchmarks built on Qt Test's `QBENCHMARK` (no new dependency),
  enabled with `-DQIFTOP_BUILD_BENCHMARKS=ON` and excluded from the default
  build and `ctest` run. Initial coverage: the connection aggregator, the
  filter mini-language evaluator, the top-K flow cap, and a full data-plane
  "pipeline tick" (cap → aggregate → filter — the empirical eager-budget
  number: ~35 ms over 100k raw flows), with deterministic synthetic inputs
  scaling to 100k flows. See `docs/HACKING.md` §5.8.
- **`QIFTOP_ENABLE_LTO` build option** — opt-in link-time optimization
  (IPO/LTO) where the toolchain supports it; warns and builds without it
  otherwise.
- **`compile_commands.json`** is now emitted by every build
  (`CMAKE_EXPORT_COMPILE_COMMANDS`) for editors, language servers and
  compilation-database tooling.
- **Phase-0 clang-tidy scaffolding** — a conservative config, opt-in
  `QIFTOP_CLANG_TIDY` build hook, local wrapper script, and a CI lane so static
  analysis can start boring and ratchet later. The baseline set is now cleaned
  and **enforced** — see Changed below.

### Changed
- **clang-tidy baseline is now gating.** The high-signal Phase-0 set
  (`bugprone-*`, `clang-analyzer-*`, `performance-*`, `portability-*`,
  `misc-*`, minus the Qt/noise traps) was fixed to zero findings, then the CI
  `clang-tidy` job flipped from report-only to enforcing (`--gate`, no more
  `continue-on-error`). Regressions in that set now fail the build.
  `bugprone-narrowing-conversions` and the `modernize-*` / `readability-*`
  style families stay parked for a later phase. The ~30 findings were real
  fixes (const-ref params, no-automatic-move returns, merged identical
  branches, qualified virtual calls in destructors, exhaustive-switch
  defaults), with two narrow `NOLINT`s where the diagnostic was a genuine
  false positive (Qt's mandated `QDBusArgument` extraction-operator signature,
  and a copy that must outlive an immediate container `erase`).

### Fixed
- **Hardened a few 32-bit-multiplication-then-widen spots** flagged by
  `bugprone-implicit-widening-of-multiplication-result` — agent config and DNS
  TTL bounds plus benchmark data generation now do the multiply in 64-bit
  before widening. These were defensive (the operands are small constants /
  test counters today), so no user-visible behavior changed, but the math is
  now overflow-safe if the inputs ever grow.

## [0.3.0] - 2026-06-17

The "all-inclusive" release: first-class **BSD support** (FreeBSD + NetBSD
client builds), deeper **process/container attribution**, and a batch of
correctness fixes. The Linux agent and the D-Bus contract remain
backward-compatible — older clients keep working, and new clients now degrade
gracefully against older agents.

### Added
- **BSD platform support** — `qiftop` (GUI), `nqiftop` (TUI) and `libqiftop`
  build on **FreeBSD 14/15 and NetBSD 11** from the same tree (the Linux-only
  agent is simply not built). In-process capture: `getifaddrs(3)` interface
  counters, **libpcap/BPF** per-flow accounting with a userspace flow table and
  SYN-based direction, a pure-`sysctl` socket→PID join for process attribution,
  and **FreeBSD jail attribution** (jailed flows tagged `runtime:jail`). See
  `docs/PORTABILITY.md` §7.
- **Attribution reason** — every flow now reports *why* it is or isn't
  attributed to a local process: `resolved`, `forwarded` (routed/NAT — no local
  owner by design), `orphaned` (TCP socket torn down) or `nosocket`. Surfaced as
  a colour-coded label in the GUI Process column, in the TUI detail panel, and
  as a new `reason:` filter field. Computed server-side (D-Bus contract
  `Version` 0.6, capability `attribution-reason`); clients derive it locally
  when talking to an agent that predates the token.
- **nqiftop** gained group collapse/expand (`h`/`l`/Enter), a grouping-aware
  flow-column header, `Ctrl-F`/`Ctrl-B` page down/up (vim/less aliases), and a
  `W` export action that prompts for a filename (`w` still auto-names).
- **Sort within groups** — a new toggle (GUI Settings → Display, and the
  nqiftop settings overlay; on by default) sorts flows *within* each group while
  the group order stays frozen at first appearance. Turning it off restores the
  classic global sort that also reorders groups by their aggregate.
- **nqiftop group-info window** — pressing `Enter` on a group header now opens a
  detail window (process `comm`/`pid`/`uid` plus on-demand `exe`/`cmdline`/`cwd`
  via `GetProcessDetails`, container scope, and the group's aggregate rates and
  flow count); fold/unfold stays on `h`/`l`/`Space`. TUI info dialogs gained an
  accent-coloured label column and word-wrapping of long values to the dialog
  width.

### Performance
- **nqiftop viewport-only rendering** — a frame now formats cells only for the
  rows visible in the scroll window instead of the whole (possibly thousands of
  rows) table; off-screen rows cost only their cheap structural pass.
- **In-process flow cap** — the in-process `ConntrackMonitor` now keeps only the
  top 4096 flows by bytes (a bounded min-heap), matching the agent's snapshot
  cap, so a busy router no longer balloons client memory.

### Fixed
- **Dual-stack (v4-mapped IPv6) attribution** — flows owned by an `AF_INET6`
  socket serving an IPv4 peer (`::ffff:a.b.c.d`, the default for many daemons:
  kdeconnect, sshd, most JVMs) were never attributed because the v6 socket key
  could not match the pure-IPv4 conntrack flow. The sock_diag indexer now also
  keys such sockets under their IPv4 form. On a typical host this raised overall
  flow attribution from ~47% to ~73%.
- **No more root-owned files in your home** — running a client privileged with a
  foreign `$HOME` (`sudo -E nqiftop`, or the GUI self-elevation re-exec) used to
  write root-owned `~/.config/qiftop/*.conf`. Settings are now loaded but not
  written when persisting would escalate into another user's config directory.
- **New client vs. old agent no longer crashes** — a newer client reading an
  older agent's shorter D-Bus struct aborted (read past the end of the
  structure). The wire readers now tolerate any shorter (append-only) struct,
  and clients demarshal the raw reply instead of the typed reply that Qt would
  reject outright on a signature mismatch.
- **nqiftop**: fixed a 100% CPU spin on stdin EOF and unresponsive input on
  FreeBSD (raw `read(2)` input path), plus garbled box-drawing on BSD curses
  (wide-char rendering).
- **`.deb` install conflict** — installing the 0.3.0 package set over an older
  `libqiftop0` failed with `Depends: libqiftop0 (= 0.2.5) but 0.3.0 is to be
  installed`. The Debian shlibs version policy is now `>=` (not exact `=`), so a
  component never pins an incompatible-but-ABI-compatible library version.

### Changed
- **Process & Container columns now show by default** in the GUI Connections
  view when the connected agent advertises attribution (the
  `process-attribution-wire` / `container-attribution-wire` capabilities) —
  the data is already on the wire, so surfacing it costs nothing. They stay
  hidden against an agent (or in-process backend) that doesn't provide
  attribution, and the per-column toggle still persists your choice. When the
  view is grouped *by process* the Process column is auto-hidden (and *by
  container* the Container column), since the value already appears in the
  group header.
- D-Bus contract `Version` → **0.6** (additive: `ConnectionDto.reason`,
  capability `attribution-reason`). `org.qiftop.NetworkAgent1` is unchanged
  otherwise; no `NetworkAgent2`.

## [0.2.5] - 2026-06-11

A performance release: hot-path optimizations on the agent and UI, all
behaviour-preserving (verified on glibc and musl).

### Performance
- **Aggregator signal coalescing** — `ConnectionAggregator` emitted a
  per-row `rowsUpdated(i,i)` during each update (thousands of granular
  signals per tick on a busy host, each driving proxy invalidation +
  repaint). It now emits one `rowsUpdated(first,last)` per maximal
  contiguous run of changed rows.
- **Single-pass netns scan** — `NetnsScanner` walked `/proc` once per
  namespace (O(netns × processes)); it now walks `/proc` once into a
  `netns→pids` map and processes only each namespace's own pids. All
  `setns` fencing and PID-reuse guards are preserved.
- **Delegate allocation reuse** — `ConnectionFlowDelegate` reuses a single
  `QTextDocument` instead of allocating one per cell per repaint frame
  (rendered output unchanged).
- **Bounded LRU route cache** — `ConntrackMonitor`'s route / ifindex caches
  evict least-recently-used entries instead of clearing wholesale on
  overflow, eliminating per-tick thrash on routers with many unique remotes.

### Tested
- New `test_aggregator_signals` verifies the coalescing (contiguous updates
  collapse to one range, sparse updates to maximal runs). Full suite (34
  tests) green on glibc and Alpine/musl.

## [0.2.4] - 2026-06-11

The "rounded-out distribution" release: desktop-environment integration,
shell completions, and broader-distro packaging — all packaging recipes
container-verified, plus first-class Alpine/musl support.

### Added
- **Freedesktop integration** under the reverse-DNS app-id
  `io.github.thecleaners.qiftop`: an **AppStream metainfo** file (visibility in
  GNOME Software / KDE Discover), a dedicated **nqiftop terminal launcher**
  (`.desktop`, `Terminal=true`), and **rasterized hicolor PNG icons**
  (16–256 px) alongside the scalable SVG. (The agent's D-Bus name
  `org.qiftop.NetworkAgent1` is a separate namespace and is unchanged.)
- **Shell completions** (bash / zsh / fish) for `qiftop`, `nqiftop`,
  `qiftop-agent`, and `check_qiftop`, packaged per component.
- **Broader-distribution packaging recipes**, each verified end-to-end in a
  container: an Arch **`PKGBUILD`** (`dist/aur/`, built with `makepkg`), a
  Fedora **COPR `.spec`** (`dist/copr/`, built with `rpmbuild`), and an Alpine
  **`APKBUILD`** (`dist/alpine/`, built with `abuild`).
- **First-class Alpine/musl support**: an **OpenRC service**
  (`dist/alpine/qiftop-agent.openrc`) that loads the nftables conntrack shim
  and supervises the agent (the OpenRC analogue of the systemd unit), and a
  CI **Alpine/musl build+test lane**. The full suite passes on musl.

### Changed
- The GUI window now advertises `setDesktopFileName(io.github.thecleaners.qiftop)`
  so Wayland associates it with the launcher.

### Fixed
- Hardened two GUI filter smoke tests (`QTRY_*` instead of fixed waits) so they
  are robust under the offscreen QPA across glibc and musl.

### Packaging / CI
- Package directories are staged 0755 regardless of the builder's umask.
- CI validates both desktop entries and the AppStream metainfo
  (`desktop-file-validate` + `appstreamcli`).

## [0.2.3] - 2026-06-10

The "test depth + integration reach" release: more of the capture path is now
unit-testable, a monitoring-plugin integration lands, and a batch of
correctness fixes from a focused bug/perf triage.

### Added
- **`check_qiftop`** — a Nagios/Icinga/Zabbix monitoring plugin (a `libqiftop`
  consumer) that samples an interface or filter-scoped flow rate, compares it
  against `--warning`/`--critical` thresholds (IEC suffixes accepted), and
  emits Nagios-format output + perfdata with exit codes 0/1/2/3. Shipped as
  the `qiftop-monitoring-plugin` package (installed under `libexec/qiftop`),
  with a `check_qiftop(1)` man page. This is the polling-check complement to
  the Prometheus exporter example.
- **In-process agent-service tests** (`test_services`) driving
  `ConnectionsService`/`InterfacesService` through the fake monitors (snapshot
  cap, dropped-flow handling, process/container/chain attribution, server-side
  direction) — no real D-Bus bus required.
- **Pure conntrack flow-orientation logic** extracted to `ConntrackOrient.h`
  (`orientConntrackFlow`) and unit-tested (`test_conntrack_orient`): inbound /
  outbound / forwarded / both-local orientation is now testable without a live
  conntrack handle.
- A **cri-o Tier-2 attribution runner** (`run-crio.sh`).

### Fixed
- **Process attribution could pick the wrong PID** for unconnected UDP sockets
  on local-key collisions (e.g. `SO_REUSEPORT`, or a wildcard `0.0.0.0:port`
  socket coexisting with a specific-address socket on the same port). The
  local 2-tuple fallback is now ambiguity-aware (a local key with more than
  one owner yields no attribution) and UDP-only; exact 4-tuple matches still
  take precedence, so connected-socket/TCP attribution is unchanged. The
  container-side scanner gained the same UDP fallback + guard.
- nqiftop CSV export (`w`) now writes the **displayed** (filtered/sorted) rows
  rather than the full raw set.
- nqiftop: the `+N more` indicator no longer paints over the cursor row; CSV
  export filenames include milliseconds to avoid same-second collisions.
- Interface rate no longer underflows to a huge bogus value on a kernel
  counter reset/wrap (rate clamps to 0, matching the connection aggregator).
- Agent snapshot cap: overflow-safe top-N-by-bytes comparison and no full
  flow-list copy when over the cap.
- Agent config: an out-of-range `hint_ttl_secs` now warns and falls back
  instead of being silently clamped.

### Performance
- `SockDiagResolver` reads each PID's start time once per refresh instead of
  repeatedly (per socket fd and per resolved flow).

Pre-release alpha tags (`v0.1-alphaN`) are intentionally omitted; their
commit history is preserved in `git log`.

## [0.2.2] - 2026-06-10

The "libqiftop consumers + polish" release: example consumers that exercise
the shared library, plus nqiftop and packaging refinements.

### Added
- **Example consumers** under `examples/` (standalone `find_package(qiftop)`
  projects, built against the installed library in CI so they can't bit-rot):
  - `prometheus-exporter` — a Prometheus/OpenMetrics `/metrics` endpoint
    (interface byte counters + per-container/process rate gauges). Alerting
    such as "is a container incessantly hogging bandwidth?" is delegated to
    Prometheus/Alertmanager via a PromQL `for:` clause — no new ecosystem to
    learn. Container/process rates are gauges (windowed with
    `max_over_time`), not fake counters over churning flows, keeping
    cardinality bounded.
  - `ndjson-connections` — per-flow NDJSON with process/container attribution.
  - `snapshot-export` — one-shot CSV/JSON dump via `util::exporter`.
  - `top-talkers` — a headless `iftop -t`-style top-N printer.
- **nqiftop view export** — `w` writes the active view (interfaces or
  connections) to a timestamped CSV via the shared exporter, with a transient
  status line.

### Changed
- **nqiftop pause is now a true freeze.** `p` snapshots the current rows and
  renders/navigates from that frozen copy, so the view no longer shifts or
  resorts under a key-driven redraw while paused (the aggregators keep
  updating in the background, so unpausing shows fresh data).

### Fixed
- The `ndjson-stream` example never called `qiftop::dbus::registerTypes()`, so
  it silently received no data at runtime; fixed (and all examples now
  register the DBus DTO metatypes).

### Packaging
- Each `.rpm` is now individually signed with `rpm --addsign` (header
  RSA/SHA256) in the dnf repo, and the published `qiftop.repo` sets
  `gpgcheck=1` — packages are verified directly, in addition to the signed
  `repomd.xml`.
- Package directories are staged 0755 regardless of the builder's umask
  (locally-built packages are reproducible and lint-clean).

## [0.2.1] - 2026-06-10

The "libqiftop + ncurses" release: the Widgets-free core is now a reusable
shared library, and qiftop gains a first-class terminal frontend.

### Added
- **`libqiftop`** — a Qt6::Core-only shared library carrying the wire DTOs,
  the DBus client proxies, the per-interface / per-connection aggregators,
  the filter mini-language, the IEC unit formatters, and JSON/CSV export.
  No Qt Widgets dependency, so it runs headless. Shipped as `libqiftop0`
  (runtime) + `libqiftop-dev` (`qiftop-libs` / `qiftop-devel` on RPM) with a
  relocatable CMake package (`find_package(qiftop)` → `qiftop::qiftop`) and a
  `qiftop.pc` pkg-config file.
- **`nqiftop`** — an ncurses frontend (the first non-Widgets consumer of
  `libqiftop`). Runs headless over SSH (Qt Core/Network/DBus + ncursesw, no
  Widgets). Features: live interface + connection views, row-spanning
  bandwidth gauges, iftop-style colour coding, grouped views (`g`:
  interface/process/container), live filter input (`/`), pause (`p`),
  vim-style `hjkl` navigation, an htop-style current-line cursor, an
  aptitude-style modal per-row detail overlay (`Enter`), Help (`?`) and
  About (`a`) overlays, selectable themes (`z`), and persisted view/settings
  with `--view` / `--group` CLI overrides.
- **Man pages** for `qiftop(1)`, `nqiftop(1)`, `qiftop-agent(8)`, and
  `qiftop-agent.conf(5)`, installed per component (gzip-compressed).
- **`[process_details]` agent config section** — the `GetProcessDetails`
  privileged-field disclosure gate is now configurable: `owner` (default),
  `permissive`, or `restricted` (with `allow_users` / `allow_groups`
  allowlists, e.g. `wheel`).

### Changed
- `MainWindow` connection/interface views are now driven by plain-`QObject`
  aggregators (`ConnectionAggregator` / `InterfaceAggregator`) extracted from
  the models, so the same aggregation logic backs both the GUI and `nqiftop`.

### Fixed
- **UDP / listener process attribution.** `SockDiagResolver` indexed each
  socket only by its full 4-tuple, so unconnected UDP sockets and TCP
  listeners (whose `idiag_dst` is `0.0.0.0:0`) never matched a live flow's
  real remote — leaving most UDP flows (e.g. ZeroTier, QUIC servers)
  unattributed. Sockets are now additionally indexed by a local-only
  2-tuple, with `resolvePid` falling back 4-tuple → exact local → wildcard
  local. On a busy host this raised overall flow attribution from ~17% to
  ~77%.
- **`GetProcessDetails` cross-UID disclosure.** The privileged
  `exe`/`cwd`/`cmdline` fields are now disclosed only to root or the process
  owner by default (was: any `netdev` caller), gated by the new
  `[process_details]` policy.

### Packaging
- `nqiftop` and `libqiftop0` are built, tested, lint-clean, and smoke-installed
  in CI; the release CD ships them in both `.deb` and `.rpm`.
- `libqiftop0` now ships a Debian `shlibs` control file and an `ldconfig`
  trigger; man pages are gzip-compressed (lintian-clean).

## [0.2] - 2026-06-10

The process/container attribution + Fedora packaging release.

### Added
- Per-flow **process + container attribution** in the agent (PID / comm /
  uid, container runtime / id / name, and the full outer→inner container
  chain), exposed over an extended DBus contract and gated behind capability
  tokens. The resolver chain combines `SockDiagResolver` (5-tuple → socket
  inode → PID), `CgroupClassifier` (`/proc/<pid>/cgroup` → runtime/id/name),
  and `NetnsScanner` (sock_diag coverage inside non-host network namespaces,
  requiring `CAP_SYS_ADMIN`). Caches are bounded and guarded by `(pid,
  starttime)` to avoid PID-reuse mistakes.
- **Nested container chain** support for stacked runtimes (for example k3s
  pods inside docker-launched k3d nodes), exposed as
  `ConnectionDto.containerChain`; the GUI shows chain depth in the Container
  column and details in tooltips.
- **Process and Container columns** in the Connections view, hidden by default
  and capability-gated on the agent's attribution wire tokens. Settings →
  Display and the header context menu control visibility.
- On-demand `GetProcessDetails(pid)` RPC for lazily fetching
  `exe`/`cmdline`/`cwd`/`startTime` instead of shipping expensive fields in
  every snapshot.
- **Filter mini-language extensions**: `pid=`, `uid=`, `comm:`,
  `runtime=docker`, `container:<haystack>`, and `chain_has:<runtime>`.
  `pid=0` intentionally selects unattributed flows.
- **Grouped Connections views**: Flat (default), By Interface, By Container,
  and By Process. Group rows aggregate rate / byte / packet sums and support
  column-header sorting.
- **Row context-menu attribution actions** for filtering or copying process,
  container, and runtime information.
- **RPM / Fedora packaging** alongside the existing `.deb`, wired into the
  release CD (`cpack -G RPM`, `%config(noreplace)` agent config, native
  scriptlets for `netdev` + systemd reload, and prerelease `~rcN` ordering).
- **Signed apt + dnf repositories** published on GitHub Pages from release
  assets; package files stay out of git.

### Changed
- **DBus contract Version `"0.5"`** (additive): `ConnectionDto` gained the
  attribution fields and nested chain; `Connections.GetProcessDetails(uint
  pid)` was added; new capability tokens advertise process/container
  attribution, chain support, netns scanning, and on-demand process details.
- Debian `postinst` auto-enrols the installing user (`$SUDO_USER` /
  `$PKEXEC_UID`) into `netdev`; users still need to log out or run
  `newgrp netdev` for the membership to take effect.
- Connections view migrated to a `QTreeView` with grouped modes (by interface
  / container / process) over a two-proxy chain. Flat mode remains a strict
  pass-through to preserve the v0.1 table layout.

### Fixed
- Per-flow throughput gauge dark fill is painted on the Connections view
  again after the `QTreeView` migration.
- Connections-view header clicks now sort grouped rows
  (`ConnectionGroupProxy::sort()` forwards and aggregates correctly).
- `/user.slice/.../user@<uid>.service` and descendants no longer
  mis-attribute as `systemd` containers.
- `ConnectionGroupProxy` avoids reset storms on source model changes; Flat
  mode preserves selection and scroll as a 1:1 pass-through.
- Container column group rows no longer mis-render as `(host)`.

### Testing / integration
- Tier-2 attribution runners ship for docker and rootful podman and run in CI
  on push-to-main / dispatch / release. k3d and naked-k8s runners remain
  local-only via the Vagrant harness, with their chain shapes pinned by unit
  fixtures.
- The Vagrant harness gained a Fedora SELinux VM that installs the real RPM,
  runs the systemd + DBus attribution path under SELinux enforcing, and audits
  qiftop AVC denials.

## [0.1] - 2026-06-08

The first qiftop stable release: a Qt 6 iftop-style Linux network monitor
shipped as two Debian packages (`qiftop` GUI client + `qiftop-agent`
privileged DBus daemon).

### Security
- Privileged child processes no longer inherit the user's `PATH`; escalated
  helpers force `PATH=/usr/sbin:/usr/bin:/sbin:/bin`.
- `HandoffServer` evicts unauthenticated incumbent peers, gates accepted peers
  with `SO_PEERCRED`, caps pre-auth and post-auth buffers, and no longer falls
  back to `/tmp` for its socket directory.
- The 256-bit handoff nonce is stored in a 0600 file and passed by path
  (`QIFTOP_HANDOFF_NONCE_FILE`) instead of exposing the secret on argv.
- `agent::loadIdleConfig` bounds schedule/timeout values before multiplying
  into milliseconds, avoiding signed-integer overflow on huge config values.
- `IdleManager::setClientHint` returns `bool`; services only count accepted
  cadence hints as activity.

### Added
- **Real-time per-interface throughput** with Qt model/view tables, row gauges,
  and tray-icon sparklines backed by libnl-3 / rtnetlink counters.
- **Per-connection flow table** from conntrack with byte / packet rates,
  direction, interface attribution, TCP state, and the filter expression bar.
- **First-class IPv4 + IPv6** capture, including separate AF_INET and AF_INET6
  conntrack dumps and the inert `inet qiftop` nftables shim.
- **System-bus DBus agent** with cadence hints, idle wind-down, snapshot caps,
  monotonic snapshot timestamps, and Version / Capabilities probing.
- **Client-side DNS resolution** with an in-process LRU cache; the agent never
  sees hostnames.
- **System tray** with live throughput sparkline and optional XDG autostart.
- **Help → Keyboard Shortcuts…** (`F1`), `Ctrl+F` filter focus, `Esc` filter
  clear, `Ctrl+C` row copy, and `Ctrl+1` / `Ctrl+2` tab switching.
- Per-table column widths and visual order persisted via
  `QHeaderView::saveState()`.
- Connections context-menu actions to show or hide flows for a selected peer.
- Empty-state placeholder on the Connections tab when no flows match.
- `QIFTOP_TESTS_SANITIZE` CMake option for per-test sanitizer builds.
- `SECURITY.md`, `test_units`, and `test_priv_escalator`.

### Changed
- GUI falls back automatically to the in-process backend with self-elevation
  and nonce-authenticated handoff when the DBus agent is unavailable or the
  user is not in `netdev`.
- Layering was kept platform-portable: kernel code lives under
  `backend/<platform>/`; UI, DBus DTOs, utilities, config, and DNS stay
  platform-agnostic for future `libqiftop` extraction.

### Fixed
- README test-count and filter-expression regex examples were refreshed.

### Packaging / install
- Two `.deb` packages built via `cpack -G DEB`: `qiftop` and `qiftop-agent`.
- Shipped `dist/conf/agent.conf` as a conffile, plus systemd unit, DBus
  activation file, system-bus policy, desktop entry, and icon.
- System-bus policy restricted agent methods and signal delivery to the
  `netdev` group; `postinst` ensured the group exists and auto-enrolled the
  installing user.

### Testing
- The v0.1 ctest suite covered pure logic, settings/autostart/export, DBus type
  round-trips, DNS cache, handoff auth, proxy models, agent config / idle
  manager, and a session-bus integration test that spawned `qiftop-agent`.
- CI also ran packaging QA (`lintian`, `desktop-file-validate`, and a Docker
  smoke install).

[0.3.0]: https://github.com/TheCleaners/qiftop/compare/v0.2.5...v0.3.0
[0.2.5]: https://github.com/TheCleaners/qiftop/compare/v0.2.4...v0.2.5
[0.2.4]: https://github.com/TheCleaners/qiftop/compare/v0.2.3...v0.2.4
[0.2.3]: https://github.com/TheCleaners/qiftop/compare/v0.2.2...v0.2.3
[0.2.2]: https://github.com/TheCleaners/qiftop/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/TheCleaners/qiftop/compare/v0.2...v0.2.1
[0.2]: https://github.com/TheCleaners/qiftop/compare/v0.1...v0.2
[0.1]: https://github.com/TheCleaners/qiftop/releases/tag/v0.1
