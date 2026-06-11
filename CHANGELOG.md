# Changelog

All notable changes to qiftop are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

## [0.2] - 2026

### Added
- Per-flow **process + container attribution** in the agent (PID / comm /
  uid, container runtime / id / name, and the full outer→inner container
  chain), exposed over an extended DBus contract and gated behind capability
  tokens. On-demand `GetProcessDetails(pid)` RPC for `exe`/`cmdline`/`cwd`.
- **RPM / Fedora packaging** alongside the existing `.deb`, wired into the
  release CD.
- **Signed apt + dnf repositories** published on GitHub Pages.

### Changed
- Connections view migrated to a `QTreeView` with grouped modes
  (by interface / container / process) over a two-proxy chain.

## [0.1] - 2025

- Initial release: Qt 6 iftop-style network monitor (GUI) with a privileged
  DBus agent (`qiftop-agent`), per-interface and per-connection live
  statistics via libnl-3 and libnetfilter_conntrack, async client-side DNS,
  IPv6 support, a system tray with live sparklines, and a filter
  mini-language.

[0.2.1]: https://github.com/TheCleaners/qiftop/compare/v0.2...v0.2.1
[0.2]: https://github.com/TheCleaners/qiftop/compare/v0.1...v0.2
[0.1]: https://github.com/TheCleaners/qiftop/releases/tag/v0.1
