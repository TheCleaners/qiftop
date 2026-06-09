# Copilot Instructions

## Project Overview

Qt6 GUI application for real-time network interface traffic monitoring (iftop-like). Uses libnl-3 on Linux for live traffic data and **Qt 6 Widgets** for the UI. **CMake only**, **C++20**.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/<app-binary>
```

Release build:
```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
```

## Architecture

Layered, platform-portable design. Two independent data streams (per-interface counters and per-connection flows) sit behind separate abstract `QObject` interfaces; UI is pure Qt Widgets driven by a model/view stack with `QSortFilterProxyModel` filters; configuration is centralized in a `Settings` object backed by `QSettings`.

```
src/
  main.cpp                                 # constructs Settings, monitors, DNS, MainWindow; --tray flag
  agent/                                   # qiftop-agent (privileged DBus daemon)
    main.cpp                               # bus name + Application wiring
    InterfacesService.{h,cpp}              # /org/qiftop/NetworkAgent1/Interfaces
    ConnectionsService.{h,cpp}             # /org/qiftop/NetworkAgent1/Connections
    IdleManager.{h,cpp}                    # adaptive poll cadence + per-client SetDesiredIntervalMs hints
  config/
    Settings.h/.cpp                        # QSettings-backed app prefs, emits changed()
  backend/
    NetworkMonitor.h/.cpp                  # abstract: per-interface stats
    ConnectionMonitor.h/.cpp               # abstract: per-flow (5-tuple) stats
    Connection.h                           # Endpoint/Connection/L4Proto/Direction data types
    linux/
      NetlinkMonitor.h/.cpp                # libnl-3 worker (server-side)
      NetlinkWorker.h/.cpp                 # libnl-3 poll loop (worker thread)
      ConntrackMonitor.h/.cpp              # libnetfilter_conntrack real impl (server-side)
    dbus/
      DBusNetworkMonitor.h/.cpp            # client-side DBus proxy
      DBusConnectionMonitor.h/.cpp         # client-side DBus proxy
  dbus/
    Types.{h,cpp}                          # DTOs + Qt marshalling; wire contract
  dns/
    DnsResolver.{h,cpp}                    # abstract async DNS resolver
    QtDnsResolver.{h,cpp}                  # QHostInfo-backed with in-process cache
  ui/
    MainWindow.{h,cpp}                     # tabs (Interfaces / Connections) + menu + toolbar + tray
    SettingsDialog.{h,cpp}                 # tabbed preferences (Monitoring/Display/DNS/Tray)
    NetworkModel.{h,cpp}                   # interfaces table model
    InterfaceFilterProxy.{h,cpp}           # loopback/down filter
    InterfaceNameDelegate.{h,cpp}          # iface name styling
    ConnectionModel.{h,cpp}                # connections table model; smoothing + direction
    ConnectionFilterProxy.{h,cpp}          # IPv6/proto/iface + filter-expression evaluator
    ConnectionHeuristics.h                 # PURE-LOGIC header: inferDirection, isForwardedFlow,
                                           # emaUpdate, emaUpdateAsym, easeOutCubic — unit-testable
    ConnectionFlowDelegate.{h,cpp}         # rich-text flow column painter
    RowGaugeDelegate.{h,cpp}               # row-spanning throughput gauge background
    TrayManager.{h,cpp}                    # system tray + live sparkline rates
  util/
    Units.h                                # IEC byte / byte-rate formatters
    ConnectionFilter.{h,cpp}               # filter mini-language: parser + AST + evaluator + helpHtml
    Autostart.{h,cpp}                      # XDG autostart entry manager (~/.config/autostart/qiftop.desktop)
    Exporter.{h,cpp}, Exportable.h         # JSON/CSV export of model rows
    HandoffClient.{h,cpp}, HandoffServer.{h,cpp}  # legacy "relaunch as admin" handoff
    Logging.{h,cpp}                        # qCInfo categories (lcVerbose etc.)
    PrivilegeEscalator.{h,cpp}             # pkexec/sudo elevation
tests/                                     # Qt6::Test unit tests, gated by QIFTOP_BUILD_TESTS
  test_direction.cpp, test_forwarded.cpp, test_ema.cpp,
  test_settings_migration.cpp, test_autostart.cpp, test_filter.cpp
```

- **Two backends, same shape**: `NetworkMonitor` emits `statsUpdated(QList<InterfaceStats>)`. `ConnectionMonitor` emits `connectionsUpdated(QList<Connection>)`. Both run capture loops in worker `QThread`s; main-thread delivery via Qt's automatic queued connections.
- **`ConntrackMonitor` is a working capture** using libnetfilter_conntrack. Must issue **separate `AF_INET` and `AF_INET6` dumps** — a single `AF_UNSPEC` dump is unreliable across kernel/lib combos. A small `inet qiftop` nftables shim is installed and loaded by the systemd unit to ensure the kernel wires up conntrack for both families on desktops that have no IPv6 ct references.
- **IPv6 first-class**: `Endpoint::address` is a `QHostAddress`. The `Show IPv6` preference filters at the view via `ConnectionFilterProxy`, never at the capture layer.
- **Async DNS**: `ConnectionModel` holds a `DnsResolver*` and re-renders rows on `resolved()` signals; cache + in-flight dedup live in `QtDnsResolver`. DNS is **always** client-side, never in the agent.
- **Settings is the single source of truth**: `MainWindow` listens to `Settings::changed` and re-applies filters/resolver/smoothing/tray state via `applySettingsToUi()`.
- **`NetworkMonitor.cpp` / `ConnectionMonitor.cpp`** exist solely to give AUTOMOC a translation unit for the abstract base classes.

## DBus contract (`org.qiftop.NetworkAgent1`)

| Path | Methods | Signals |
|------|---------|---------|
| `/.../Interfaces`  | `GetInterfaces()`, `SetDesiredIntervalMs(u)` | `StatsChanged` |
| `/.../Connections` | `GetConnections()`, `SetDesiredIntervalMs(u)` | `ConnectionsChanged`, `PermissionDenied`, `accountingChanged` |

Wire format is **native Qt marshalling, NOT JSON** (`a(yysqysqtttts)` for connections — see `src/dbus/Types.cpp`). Breaking DTO changes ⇒ bump to `NetworkAgent2`.

**Idle manager gotcha**: the agent only resets activity on incoming method calls. Subscribing to signals alone is NOT enough — clients must heartbeat via `SetDesiredIntervalMs` every ≤ `hintTtlMs/2` (default 4s) or polling slows at 30s and pauses at 60s. The GUI does this in `MainWindow::applySettingsToUi`.

## Key Conventions

- **C++20**, `CMAKE_CXX_EXTENSIONS OFF`. Use idioms freely: designated initializers, `[[nodiscard]]`, `constexpr`, `std::ranges`, `if (auto x = ...; cond)`, default-generated `operator==`, etc.
- **Qt 6 only** — modern signal/slot syntax (`&Class::signal`), `Qt::Foo` namespace in CMake.
- **Privileged data via DBus agent**: `qiftop-agent` runs as root (system bus name `org.qiftop.NetworkAgent1`). The UI probes for it at startup and uses `DBusNetworkMonitor` / `DBusConnectionMonitor` when reachable; otherwise it falls back to in-process `NetlinkMonitor` / `ConntrackMonitor` (and the `PrivilegeEscalator/Handoff` "Relaunch as administrator" path).
- **Dependency injection over singletons**: `Settings`, monitors, and the DNS resolver are constructed in `main()` and passed to `MainWindow`. Don't introduce `Foo::instance()` accessors.
- **Modern CMake**: `target_*` commands only; backend implementations are separate `STATIC` library targets linked into the main executable.
- **Platform isolation**: `#ifdef Q_OS_*` / OS headers belong only in `backend/<platform>/`. UI, `dns/`, `config/`, and `util/` must remain platform-agnostic.
- **libnl-3**: Use `libnl-route-3` (`rtnl_*`). Per-flow data uses libnetfilter_conntrack — not the deprecated iproute2 `libnetlink.h`.
- **Threading**: Each monitor owns its `QThread`. UI mutations only on the main thread. Cross-thread delivery via auto-queued Qt signals.
- **Model/view, not custom widgets**: For tabular data, add columns to the relevant model. Always provide a numeric `SortRole` distinct from `DisplayRole` for sortable rate/byte columns.
- **Settings keys**: Defined as `constexpr auto k...` constants at the top of `Settings.cpp`. Always add a getter, setter, `changed()` emission, and a row in `SettingsDialog` for any new preference.
- **Units**: Always format byte counts/rates through `util::formatBytes` / `util::formatByteRate`. No ad-hoc formatters in UI code.
- **DNS hostnames**: Address columns must go through `ConnectionModel::displayAddress`.
- **Pure-logic header extraction pattern**: when a helper function in a UI class is mostly logic on plain types, extract it into a header in the same folder (see `ui/ConnectionHeuristics.h`) so unit tests can exercise it without instantiating the model/view. Pair with a test under `tests/`.
- **Per-connection rate smoothing is a 3-layer pipeline**: raw (drives reference / Max columns / gauge denom) → target (symmetric EMA at τ) → display (easeOutCubic tween advanced by `m_smoothingTick` at `max(100ms, pollMs/4)`). Don't conflate the three; never let smoothed values feed the reference.
- **`Connection::key()` includes Direction** so aggregated inbound/outbound rows for the same peer don't collide.
- **No qmake**: `.pro` files and qmake-specific APIs are not used.

## Toolbar / Qt UI gotchas

- **`QToolBar` widget stretching**: a `QWidget` container added with `addWidget()` defaults to `QSizePolicy::Preferred` horizontally — the toolbar can stretch it. If a child has a `maximumWidth` cap, slack lands in layout spacing between siblings (visible as huge gaps). Fix: set the container's horizontal policy to `QSizePolicy::Maximum`. To right-anchor a widget group, insert an explicit expanding spacer widget **before** it.
- **`QToolTip::showText()` is wrong for click-to-summon help** — it has its own dismiss timer and is killed by mouse leaves / focus changes. Use a `QFrame` with the `Qt::Popup` window flag (see `MainWindow::showFilterHelp()`).
- **`QKeySequence::Quit == Ctrl+Q` on Linux** — binding both `QKeySequence::Quit` and a literal `"Ctrl+Q"` causes "Ambiguous shortcut overload" warnings. For shortcuts that must work with the menu bar hidden, use a single `setShortcut(QKeySequence("Ctrl+Q"))` plus `Qt::ApplicationShortcut`.
- **`QStandardPaths::setTestModeEnabled(true)`** overrides `XDG_CONFIG_HOME` (redirects to a per-app sandbox) — defeats manual env overrides in tests. Use `qputenv("XDG_CONFIG_HOME", ...)` without it.
- **`Q_DECLARE_METATYPE(QList<T>)`** plus `qRegisterMetaType<QList<T>>()` in `main()` is required if you want to retrieve `QList<T>` via `QVariant`/model roles cross-thread.

## Filter expression mini-language

The Connections view supports a filter mini-language implemented in `src/util/ConnectionFilter.{h,cpp}` (pure, no Qt model/view dep). Grammar: boolean (`and`/`or`/`not`, parens), string fields (`proto`, `src`, `dst`, `host`, `iface`, `family`, `direction`), numeric fields (`sport/dport/port`, `bytes_in/out/total`, `pkts_in/out/total`, `rate_in/out/total`), ops (`: = != ~ < <= > >=`), byte suffixes (`K/Ki/M/Mi/G/Gi/T/Ti`). Evaluated against a `qiftop::filter::Context` built from model roles. See `helpHtml()` for the user-facing syntax sheet.

## Dependencies

- Qt 6 (`Core`, `Widgets`, `Network`, `DBus`, optionally `Test` for `-DQIFTOP_BUILD_TESTS=ON`)
- libnl-3 + libnl-route-3 (`libnl-3-dev`, `libnl-route-3-dev` on Debian/Ubuntu), discovered via `pkg-config`
- libnetfilter_conntrack (`libnetfilter-conntrack-dev`) for per-connection accounting
- `nftables` (Recommends) — agent loads an inert `inet qiftop` shim to ensure v4+v6 conntrack tracking
- Linux kernel headers (`<linux/if.h>`, `<linux/rtnetlink.h>`) for the Linux backend

## Installing the privileged agent

```bash
sudo cmake --install build
sudo systemctl daemon-reload    # only if you want pin-on
# Activated on demand via DBus; or:
sudo systemctl start qiftop-agent
```

Verify with:
```bash
busctl --system call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces GetInterfaces
```

The `qiftop` .deb has a `POST_BUILD` cpack hook on the **agent** target only — client-only edits need `cd build && cpack -G DEB && sudo dpkg -i qiftop_0.1_amd64.deb` (or `touch src/agent/main.cpp` first to trigger).

## Adding a New Platform Backend

1. Create `src/backend/<platform>/` with classes inheriting `NetworkMonitor` and `ConnectionMonitor`.
2. Add a `CMakeLists.txt` building a `STATIC` library named `backend_<platform>` (link `Qt6::Core Qt6::Network` PUBLIC).
3. In root `CMakeLists.txt`, extend the `if(CMAKE_SYSTEM_NAME ...)` block to include the subdirectory, link it, and set the matching `BACKEND_<PLATFORM>` compile definition.
4. In `main.cpp`, add a matching `#ifdef BACKEND_<PLATFORM>` block to instantiate the concrete monitors.

## Adding a New Preference

1. Add member + getter/setter + `m_store` key in `config/Settings.{h,cpp}` and emit `changed()` from the setter.
2. Add a widget row to the relevant tab in `ui/SettingsDialog.cpp` (read from `Settings` on construct, write on `apply()`).
3. React to it in `MainWindow::applySettingsToUi()` (or wherever the preference takes effect).

## Adding a unit test

Tests are Qt6::Test, one executable per `.cpp`, gated by `option(QIFTOP_BUILD_TESTS ON)`. Add an entry via `qiftop_add_test(<name> <name>.cpp [extra-src...])` in `tests/CMakeLists.txt`. For tests covering UI logic, first extract the pure logic into a header in the same folder (see `ui/ConnectionHeuristics.h` pattern). Run with `ctest --test-dir build --output-on-failure`.

## Further reading

- **[`docs/HACKING.md`](../docs/HACKING.md)** — recipe-level cookbook: build/run/debug recipes, common dev tasks, debugging gotchas. Update when changing dev loops.
- **[`AGENTS.md`](../AGENTS.md)** — architecture reference + changelog. Append a changelog entry for every non-trivial change.

