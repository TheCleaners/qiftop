# Copilot Instructions

## Project Overview

`qiftop` is a C++20 / CMake-only Linux network-monitoring suite built on Qt 6. It now has four shipped components plus example consumers:

- **`qiftop`** — Qt 6 Widgets GUI client (`src/main.cpp`, `src/ui/`).
- **`nqiftop`** — ncurses TUI for SSH/headless use (`src/tui/`), built when `QIFTOP_BUILD_TUI=ON` and ncursesw is found.
- **`qiftop-agent`** — privileged DBus daemon (`src/agent/`) on `org.qiftop.NetworkAgent1`; the only component that talks to the kernel.
- **`libqiftop`** — Widgets-free shared data library (`qiftoplib`, installed as `libqiftop.so.0`) for Qt6 Core/Network/DBus consumers; `find_package(qiftop)` provides `qiftop::qiftop`.
- **`examples/`** — standalone `find_package(qiftop)` consumers: NDJSON streams, snapshot export, top-talkers, and a Prometheus exporter.

The agent collects per-interface counters via libnl-route-3 and per-flow accounting via libnetfilter_conntrack, enriches flows with server-side process/container attribution, and publishes snapshots over DBus. GUI/TUI clients use `DBus*Monitor` proxies when the agent is reachable and can fall back to in-process Linux backends (the GUI also has self-elevation); examples stream from the agent over DBus only.

## Build / Run

Developer build (disable auto `.deb` regeneration unless you need it):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DQIFTOP_AUTO_PACKAGE=OFF
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Run during development:

```bash
# Session-bus agent: no system install; conntrack may warn without privileges.
./build/qiftop-agent --session --verbose -c dist/conf/agent.conf

# GUI client
./build/qiftop

# TUI client, if ncursesw was found and nqiftop was built
./build/nqiftop --session
```

Release/package builds:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
(cd build-release && cpack -G DEB)   # Debian/Ubuntu packages
(cd build-release && cpack -G RPM)   # Fedora/RPM packages, on an RPM-capable host
```

Install locally with `sudo cmake --install build-release`; the system agent is DBus/systemd activated on demand, or can be started with `sudo systemctl start qiftop-agent`. Signed apt + dnf repositories are published from release assets via GitHub Pages (`dist/repo/`, `.github/workflows/pages.yml`).

Build examples after installing `libqiftop-dev` / `qiftop-devel`:

```bash
cd examples/ndjson-stream
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr
cmake --build build
./build/qiftop-ndjson --session
```

## Architecture

Layered, platform-portable design: kernel access is isolated in the agent/Linux backend; reusable data processing lives in `libqiftop`; frontends adapt that data to Widgets, ncurses, or external tools. Two independent streams sit behind abstract `QObject` interfaces: `NetworkMonitor::statsUpdated(QList<InterfaceStats>)` and `ConnectionMonitor::connectionsUpdated(QList<Connection>)`.

```
src/
  main.cpp                         # qiftop GUI entry point; probes agent, wires Settings/DNS/MainWindow
  agent/                           # qiftop-agent: DBus Application, services, config, idle/cadence, attribution
  aggregate/                       # libqiftop aggregators: rates, smoothing, grouping-friendly row state
  backend/
    NetworkMonitor.{h,cpp}         # abstract per-interface stream
    ConnectionMonitor.{h,cpp}      # abstract per-flow stream
    Connection.h                   # Endpoint/Connection/L4Proto/Direction data types
    ProcessResolver*.h/.cpp        # pid/process/container attribution abstractions/factory
    PlatformInfo.{h,cpp}           # host facts behind a portable API
    dbus/                          # client-side DBusNetworkMonitor / DBusConnectionMonitor
    linux/                         # libnl, nf_conntrack, sock_diag, cgroup/netns, /proc implementations
    null/                          # null process resolver fallback
  config/Settings.{h,cpp}          # QSettings-backed prefs (Qt Core only)
  dbus/Types.{h,cpp}               # DTOs + Qt marshalling; wire contract
  dns/                             # client-side async reverse DNS cache
  tui/                             # nqiftop ncurses frontend; no Qt Widgets
  ui/                              # qiftop Widgets frontend: MainWindow, models, proxies, delegates, tray
  util/                            # Logging, Units, ConnectionFilter, Exporter, Autostart, elevation/handoff
examples/                          # standalone libqiftop consumers using find_package(qiftop)
dist/                              # systemd/DBus policy, config, desktop, man pages, deb/rpm/repo assets
tests/                             # Qt6::Test unit/integration tests, gated by QIFTOP_BUILD_TESTS
```

- **Agent boundary**: `qiftop-agent` owns privileged kernel access and runs as root/systemd with `CAP_NET_ADMIN`, `CAP_SYS_PTRACE`, `CAP_DAC_READ_SEARCH`, and `CAP_SYS_ADMIN` (for netns `setns`). It exposes `/Interfaces` and `/Connections` on `org.qiftop.NetworkAgent1`.
- **libqiftop boundary**: no Qt Widgets. It contains DTOs, DBus client proxies, abstract monitor types, settings/DNS helpers, aggregators, the filter mini-language, IEC unit formatters, and JSON/CSV export. Installed headers support `find_package(qiftop)` → `qiftop::qiftop` and pkg-config.
- **Frontend boundary**: `qiftop_ui` (static) contains all QWidget/model/delegate/tray/elevation code; `nqiftop` uses `QCoreApplication` + ncurses and reuses libqiftop aggregators. External examples link only `qiftop::qiftop` and stream from the agent over DBus.
- **Two backends, same shape**: DBus proxies and in-process Linux monitors both implement `NetworkMonitor` / `ConnectionMonitor`. Linux capture loops run in worker `QThread`s; cross-thread delivery uses Qt queued signals.
- **Process/container attribution**: server-side resolver chain uses sock_diag, `/proc`, cgroup parsing, and optional netns scanning. UI/client code consumes wire fields/capability tokens; do not duplicate resolver logic in frontends.
- **Conntrack capture**: issue separate `AF_INET` and `AF_INET6` dumps; a single `AF_UNSPEC` dump is unreliable across kernel/lib combinations. The systemd unit loads the inert `inet qiftop` nftables shim for v4+v6 conntrack accounting.
- **IPv6 first-class**: `Endpoint::address` is a `QHostAddress`. The `Show IPv6` preference filters at the view/proxy layer, never at capture.
- **Async DNS**: DNS is always client-side, never in the agent. GUI models and libqiftop aggregators use `DnsResolver` / `QtDnsResolver` and re-render on `resolved()`.
- **Settings source of truth**: `Settings` is QSettings-backed and emits `changed()`; `MainWindow::applySettingsToUi()` is the GUI's central reapply point.

## DBus contract (`org.qiftop.NetworkAgent1`)

| Path | Methods | Signals | Properties |
|------|---------|---------|------------|
| `/.../Interfaces`  | `GetInterfaces()`, `SetDesiredIntervalMs(u)` | `StatsChanged(t, a(...))`, `CadenceChanged(u)` | `Version`, `Capabilities` |
| `/.../Connections` | `GetConnections()`, `GetProcessDetails(u)`, `SetDesiredIntervalMs(u)` | `ConnectionsChanged(t, a(...))`, `PermissionDenied`, `AccountingChanged` | |

Wire format is **native Qt marshalling, NOT JSON** (`a(yysqysqttttsyuyuussssa(sss))` for connections — 22 outer fields including process/container attribution and nested chain; see `src/dbus/Types.cpp` and the authoritative contract in `AGENTS.md §4`). Breaking DTO changes ⇒ bump to `NetworkAgent2`.

**Idle manager gotcha**: subscribing to signals is not activity. Clients must heartbeat via `SetDesiredIntervalMs` (GUI: `MainWindow::refreshAgentHeartbeat`; TUI: its heartbeat timer; examples should do the same) or the agent slows/pauses according to `IdleManager`.

## Key Conventions

- **C++20**, `CMAKE_CXX_EXTENSIONS OFF`. Use idioms freely: designated initializers, `[[nodiscard]]`, `constexpr`, `std::ranges`, `if (auto x = ...; cond)`, default-generated `operator==`, etc.
- **Qt 6 only** — modern signal/slot syntax (`&Class::signal`), `Qt::Foo` namespace in CMake. `libqiftop` is Qt6 Core/Network/DBus only; Widgets stay in `qiftop_ui` / `src/ui`.
- **Privileged data via DBus agent**: prefer `DBusNetworkMonitor` / `DBusConnectionMonitor`. In-process `NetlinkMonitor` / `ConntrackMonitor` are fallbacks/debug paths and may need elevation.
- **Dependency injection over singletons**: construct `Settings`, monitors, aggregators, and DNS resolvers at the entry point and pass them in. Don't introduce `Foo::instance()` accessors.
- **Modern CMake**: `target_*` commands only. `qiftoplib` is the shared reusable library; `qiftop_ui` is the Widgets-only static layer; platform implementations are separate targets.
- **Layering**: `backend/` does not include `agent/`, `ui/`, or `dbus/Types.h`; `agent/` does not include `ui/`; `ui/` does not include `backend/linux/*`; `util/`, `dbus/`, `aggregate/`, and portable `backend/` code must not depend on Qt Widgets.
- **Platform isolation**: OS headers belong only in `backend/<platform>/` or `backend/PlatformInfo.cpp`. Portable code must compile without Linux headers.
- **libnl / conntrack**: use libnl-route-3 (`rtnl_*`) for interface stats and libnetfilter_conntrack for per-flow data — not iproute2 `libnetlink.h`.
- **Threading**: capture backends own worker `QThread`s. UI mutations only on the main thread; cross-thread delivery via Qt auto-queued signals.
- **Model/view in GUI, aggregators in lib**: tabular UI changes belong in models/proxies/delegates; shared rate/row logic belongs in `src/aggregate`. Always provide numeric `SortRole` values distinct from `DisplayRole` for sortable bytes/rates.
- **Settings keys**: define as `constexpr auto k...` constants in `Settings.cpp`. Add getter, setter, `changed()` emission, and a SettingsDialog row for new GUI preferences.
- **Units**: always format byte counts/rates through `util::formatBytes` / `util::formatByteRate`.
- **DNS hostnames**: GUI address columns go through `ConnectionModel::displayAddress`; non-GUI consumers should use aggregator/DNS helpers rather than blocking lookups.
- **Pure-logic extraction**: when helper logic is mostly plain data, put it in a Widgets-free header/source (`src/aggregate`, `src/util`, or a pure UI-adjacent header like `ConnectionHeuristics.h`) and add a QtTest.
- **Per-connection rate smoothing**: raw (reference / Max columns / gauge denominator) → target (symmetric EMA) → display (easeOutCubic tween). Never feed smoothed values back into the raw reference.
- **`Connection::key()` includes Direction** so aggregated inbound/outbound rows for the same peer don't collide.
- **No qmake**: `.pro` files and qmake-specific APIs are not used.

## Toolbar / Qt UI gotchas

- **`QToolBar` widget stretching**: a `QWidget` container added with `addWidget()` defaults to `QSizePolicy::Preferred` horizontally — the toolbar can stretch it. If a child has a `maximumWidth` cap, slack lands in layout spacing between siblings (visible as huge gaps). Fix: set the container's horizontal policy to `QSizePolicy::Maximum`. To right-anchor a widget group, insert an explicit expanding spacer widget **before** it.
- **`QToolTip::showText()` is wrong for click-to-summon help** — it has its own dismiss timer and is killed by mouse leaves / focus changes. Use a `QFrame` with the `Qt::Popup` window flag (see `MainWindow::showFilterHelp()`).
- **`QKeySequence::Quit == Ctrl+Q` on Linux** — binding both `QKeySequence::Quit` and a literal `"Ctrl+Q"` causes "Ambiguous shortcut overload" warnings. For shortcuts that must work with the menu bar hidden, use a single `setShortcut(QKeySequence("Ctrl+Q"))` plus `Qt::ApplicationShortcut`.
- **`QStandardPaths::setTestModeEnabled(true)`** overrides `XDG_CONFIG_HOME` (redirects to a per-app sandbox) — defeats manual env overrides in tests. Use `qputenv("XDG_CONFIG_HOME", ...)` without it.
- **`Q_DECLARE_METATYPE(QList<T>)`** plus `qRegisterMetaType<QList<T>>()` in each executable is required if you want to retrieve `QList<T>` via `QVariant`/model roles cross-thread.

## Filter expression mini-language

The Connections view and libqiftop consumers share `src/util/ConnectionFilter.{h,cpp}` (pure, no Qt model/view dep). Grammar: boolean (`and`/`or`/`not`, parens), string fields (`proto`, `src`, `dst`, `host`, `iface`, `family`, `direction`, `comm`, `runtime`, `container`, `chain_has`), numeric fields (`sport/dport/port`, `bytes_in/out/total`, `pkts_in/out/total`, `rate_in/out/total`, `pid`, `uid`), ops (`: = != ~ < <= > >=`), byte suffixes (`K/Ki/M/Mi/G/Gi/T/Ti`). Multi-haystack fields (`host`, `container`, `chain_has`) split their value into multiple lines and use any-match for `:`/`=`/`~`, all-match for `!=`. See `helpHtml()` for the user-facing syntax sheet.

## Dependencies

- Qt 6 (`Core`, `Widgets`, `Network`, `DBus`, optionally `Test` for `-DQIFTOP_BUILD_TESTS=ON`)
- ncursesw for `nqiftop` (`QIFTOP_BUILD_TUI=ON`; target is skipped with a warning if not found)
- libnl-3 + libnl-route-3 (`libnl-3-dev`, `libnl-route-3-dev` on Debian/Ubuntu), discovered via `pkg-config`
- libnetfilter_conntrack (`libnetfilter-conntrack-dev`) for per-connection accounting
- `nftables` (Recommends) — agent loads an inert `inet qiftop` shim to ensure v4+v6 conntrack tracking
- Linux kernel headers (`<linux/if.h>`, `<linux/rtnetlink.h>`, sock_diag/netns headers) for the Linux backend
- `rpm-build` / distro RPM tooling only when producing `.rpm` packages

## Installing the privileged agent

```bash
sudo cmake --install build-release
sudo systemctl daemon-reload
# Activated on demand via DBus; or:
sudo systemctl start qiftop-agent
```

Verify with:

```bash
busctl --system call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces GetInterfaces
```

The system-bus policy gates access to the `netdev` group. Package post-install scripts create the group and add the installing user; users may need to log out/in or run `newgrp netdev`.

## Adding a New Platform Backend

1. Create `src/backend/<platform>/` with classes inheriting `NetworkMonitor` and `ConnectionMonitor` (and process-resolver pieces if applicable).
2. Add a CMake target for that backend and link the needed Qt/lib dependencies.
3. Extend the root `CMakeLists.txt` platform block to add the backend, link it into `qiftoplib`, and set the matching `BACKEND_<PLATFORM>` compile definition.
4. Add matching `#ifdef BACKEND_<PLATFORM>` construction paths in `src/main.cpp` and `src/tui/main.cpp` if the frontend should support in-process fallback.

## Adding a New Preference

1. Add member + getter/setter + `m_store` key in `config/Settings.{h,cpp}` and emit `changed()` from the setter.
2. Add a widget row to the relevant tab in `ui/SettingsDialog.cpp` (read from `Settings` on construct, write on `apply()`).
3. React to it in `MainWindow::applySettingsToUi()` or the relevant TUI/libqiftop consumer path.

## Adding a unit test

Tests are Qt6::Test, one executable per `.cpp`, gated by `option(QIFTOP_BUILD_TESTS ON)`. Add an entry via `qiftop_add_test(<name> <name>.cpp [extra-src...])` in `tests/CMakeLists.txt`. For shared logic, keep the implementation Widgets-free where possible and test it without instantiating the GUI. Run with `ctest --test-dir build --output-on-failure`.

## Further reading

- **[`AGENTS.md`](../AGENTS.md)** — detailed architecture reference: layering rules, DBus contract, config/security, testing, release/package conventions. Keep it in sync with non-trivial code changes.
- **[`docs/HACKING.md`](../docs/HACKING.md)** — recipe-level cookbook: build/run/debug recipes, common dev tasks, debugging gotchas. Update when changing dev loops.
- **[`docs/LIBQIFTOP.md`](../docs/LIBQIFTOP.md)** — reusable libqiftop data-plane guide for examples and external consumers.
