# AGENTS.md — qiftop developer & contributor guide

A living document for humans and LLM-driven agents working on qiftop. Keep
each change in lock-step with the code so it stays usable. If you change a
public type, a build invariant, or a layering rule, update the relevant
section in the same commit.

---

## 1. What is qiftop?

`qiftop` is a Qt 6 iftop-style Linux network monitor. It ships as two
Debian packages built from this single tree:

| Package        | Binary           | Privilege               | Talks to                                     |
|----------------|------------------|-------------------------|----------------------------------------------|
| `qiftop`       | `qiftop`         | Unprivileged GUI client | `qiftop-agent` over DBus (system or session) |
| `qiftop-agent` | `qiftop-agent`   | `CAP_NET_ADMIN` daemon  | Linux kernel (libnl-3, libnetfilter_conntrack) |

The agent is the only component that touches the kernel. The GUI is a pure
DBus consumer (with optional self-elevation as a fallback when the agent
isn't installed). DNS resolution and any other "soft" enrichment happens
strictly client-side.

---

## 2. Source layout

```
src/
├── agent/                    # privileged DBus daemon (qiftop-agent)
│   ├── main.cpp              # config load, bus name, wires services + IdleManager
│   ├── InterfacesService.{h,cpp}
│   ├── ConnectionsService.{h,cpp}
│   └── IdleManager.{h,cpp}   # adaptive polling cadence + per-client hints
├── backend/                  # backend interfaces + platform impls
│   ├── NetworkMonitor.{h,cpp}      # abstract: per-interface stats
│   ├── ConnectionMonitor.{h,cpp}   # abstract: per-flow stats
│   ├── linux/                      # libnl + nf_conntrack impls (server-side)
│   │   ├── NetlinkMonitor.{h,cpp}, NetlinkWorker.{h,cpp}
│   │   └── ConntrackMonitor.{h,cpp}
│   └── dbus/                       # client-side DBus proxies (used by GUI)
│       ├── DBusNetworkMonitor.{h,cpp}
│       └── DBusConnectionMonitor.{h,cpp}
├── dbus/Types.{h,cpp}        # DTOs + Qt marshalling for the wire format
├── ui/                       # MainWindow, models, delegates, tray
├── util/                     # Logging, HandoffServer/Client (legacy elevation)
└── main.cpp                  # GUI entry point

dist/
├── conf/agent.conf           # ships to /etc/qiftop/agent.conf (Debian conffile)
├── dbus/                     # bus policy + system-service activation file
├── debian/                   # postinst, postrm, conffiles
├── desktop/qiftop.desktop, qiftop.svg
└── systemd/qiftop-agent.service
```

### Layering rules

1. **`backend/` does not include `agent/` or `ui/` or `dbus/Types.h`.**
   Backends speak in their own value types (`InterfaceStats`, `Connection`).
   The agent translates them to DTOs at the boundary.
2. **`agent/` does not include `ui/`.** Period.
3. **`ui/` does not include `backend/linux/*`.** It uses the abstract
   interfaces (resolved at runtime to either a `DBus*Monitor` or, when
   self-elevated, the platform monitor).
4. **`dbus/Types.h` is the wire contract.** Changing it is a breaking change
   for every installed client; bump a version or add a new method.

---

## 3. Build, run, package

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run the agent against the session bus (no root needed, conntrack will warn)
./build/qiftop-agent --session --verbose -c dist/conf/agent.conf

# Run the GUI
./build/qiftop

# Build both .debs
cd build && cpack -G DEB
```

CPack quirk worth remembering: component-specific keys use the **upper-cased
component name verbatim, hyphens kept literal** —
`CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_*`.

---

## 4. The DBus contract

Well-known name: `org.qiftop.NetworkAgent1` (system bus in production;
`--session` for development).

| Object path                                  | Interface                                  | Methods                                                   | Signals                                                |
|----------------------------------------------|--------------------------------------------|-----------------------------------------------------------|--------------------------------------------------------|
| `/org/qiftop/NetworkAgent1/Interfaces`       | `org.qiftop.NetworkAgent1.Interfaces`      | `GetInterfaces()`, `SetDesiredIntervalMs(u)`              | `StatsChanged`                                         |
| `/org/qiftop/NetworkAgent1/Connections`      | `org.qiftop.NetworkAgent1.Connections`     | `GetConnections()`, `SetDesiredIntervalMs(u)`             | `ConnectionsChanged`, `PermissionDenied`, `accountingChanged` |

DTOs live in `src/dbus/Types.h`. The connection signature is
`a(yysqysqtttts)` (family, l3proto, src, sport, dst, dport, l4proto, state,
bytes_out, pkts_out, bytes_in, pkts_in, iface).

### Cadence control

* The agent has a built-in baseline cadence (`poll.base_interval_ms`, 1 s).
* Any client may call `SetDesiredIntervalMs(uint ms)` on either service to
  request a faster cadence. The agent uses `min()` across all live hints,
  clamped to `poll.min_interval_ms` (100 ms by default).
* Hints expire after `idle.hint_ttl_secs` (default 10 s). Clients should
  re-assert at ~half-TTL while they want the boost. There is no
  `NameOwnerChanged` plumbing — TTL handles disconnects.
* If no method calls arrive for `idle.timeout_secs`, polling is paused
  entirely (no signals, near-zero CPU). The first incoming call wakes it.

---

## 5. Configuration

`/etc/qiftop/agent.conf` — INI parsed by `QSettings(IniFormat)`, read once
at startup. See the shipped file (`dist/conf/agent.conf`) for the
authoritative documentation of every key. Marked as a Debian conffile so
user edits survive package upgrades.

Override path: `qiftop-agent --config <path>`.

---

## 6. Testing strategy

Tests live in `tests/`, gated by `option(QIFTOP_BUILD_TESTS ON)` (on by
default; turn off for distro builds that don't want QtTest in the build
deps). Each test is its own executable so a single test's crash doesn't
take the rest down. Run with `ctest --test-dir build --output-on-failure`.

### 6.1 Tiers

| Tier              | What                                                  | Privilege     | Where         |
|-------------------|-------------------------------------------------------|---------------|---------------|
| **unit**          | Pure logic, no I/O, no DBus, no kernel.               | none          | `tests/`      |
| **integration**   | Spin up `qiftop-agent --session` + drive over DBus.   | none          | (not yet)     |
| **end-to-end**    | Real system bus, real conntrack. Manual / CI runner.  | root          | (not yet)     |

### 6.2 What's currently covered

| Test                       | Subject                                                           |
|----------------------------|-------------------------------------------------------------------|
| `test_direction`           | `inferDirection` (ephemeral-port + local-end fallback)            |
| `test_forwarded`           | `isForwardedFlow` heuristic                                       |
| `test_ema`                 | `emaUpdate`, `easeOutCubic`                                       |
| `test_filter`              | Filter mini-language parser + evaluator (every field/op)          |
| `test_settings_migration`  | `Settings` legacy-key migration logic                             |
| `test_autostart`           | XDG autostart file lifecycle (`util/Autostart`)                   |

### 6.3 Gaps worth filling

1. **`dbus::Types` round-trip** — `Connection` ↔ `ConnectionDto` ↔
   `QDBusArgument`. Pure marshalling; should be trivial to test.
2. **`IdleManager` cadence** — uses `QElapsedTimer` + `QTimer`. Pattern:
   inject fake monitors via the existing `NetworkMonitor` /
   `ConnectionMonitor` abstract interfaces; exercise base cadence,
   slow-down at 30s / pause at 60s, wake on `noteActivity()`, hint TTL,
   `min()` across multiple hints, clamp at `minIntervalMs`.
3. **`ConnectionFilterProxy` / `InterfaceFilterProxy` visibility** —
   empty iface set, named ifaces, the empty-string sentinel for
   unattributed flows, proto toggles, expression-filter wiring.
4. **Integration** — spawn agent on a private session bus, assert
   `GetInterfaces` returns non-empty within a 1s deadline, assert
   `SetDesiredIntervalMs(250)` produces `StatsChanged` events at ≥3 Hz
   for the next 2s.

### 6.4 Refactors that would unblock more testing

1. **Extract `loadIdleConfig` from `src/agent/main.cpp` into
   `agent/Config.{h,cpp}`** so the INI parser can be unit-tested
   without spawning the agent. Currently file-static.
2. **`main.cpp` glue → `qiftop::agent::Application` class.** Move bus
   setup, service registration, and IdleManager wiring out of `main()`
   into a small RAII class that takes a `QDBusConnection` + the two
   monitor pointers (via the abstract interfaces). Then integration
   tests can construct the application against a private bus.
3. **Extract `ConntrackMonitor::Worker`'s per-flow diff math** into a
   free function `computeDeltas(prev, current) -> QList<Connection>` so
   the diff/accounting tests don't need a live conntrack handle.
4. **Add `tests/fakes/FakeNetworkMonitor.{h,cpp}`** emitting canned
   `statsUpdated` signals on demand, to drive `IdleManager` and the
   services from tests.

### 6.5 CI

`.github/workflows/ci.yml` runs the full test suite under
`dbus-run-session` with `QT_QPA_PLATFORM=offscreen` on a matrix of
ubuntu-22.04 + ubuntu-24.04 × Debug + Release. `HOME` is redirected to
`$RUNNER_TEMP/home` so QSettings/Autostart tests don't trample the
runner user. See HACKING.md §5.5 for the test-writing conventions.

---

## 7. Coding conventions

* Qt 6, C++20.
* Logging: `qCInfo(lcVerbose) << ...` (see `src/util/Logging.h`). Anything
  off the happy path goes to `qWarning()` / `qCritical()` directly.
* `QStringLiteral` only with literal tokens; for `constexpr auto kFoo = "..."`
  use `QString::fromLatin1(kFoo)`.
* DBus methods on services must be exported with
  `QDBusConnection::ExportAllContents` (we have `public slots:` methods,
  not just scriptable ones).
* `protected QDBusContext` on a service class lets you call
  `calledFromDBus() / message().service()` to get the caller's unique name.
* Backend `setPollIntervalMs(int ms)` semantics: `ms <= 0` means "pause the
  timer, emit nothing"; positive values set the interval and (re)start.

---

## 8. Release / versioning

* `project(... VERSION 0.1)` in `CMakeLists.txt` is the single source of
  truth. Both .debs and the `CPACK_PACKAGE_VERSION` derive from it.
* The DBus interface name (`org.qiftop.NetworkAgent1`) carries the
  contract version. **If you make a breaking change to a DTO or to a
  method signature, bump to `NetworkAgent2` and keep the old one alive
  for one release.**
* User-facing config (`/etc/qiftop/agent.conf`) is a conffile — additions
  are fine, removals/renames are not. Always keep loader code tolerant
  of unknown keys.

---

## 9. Changelog discipline for this file

When making a non-trivial change, append a one-line entry below.

**Companion document — keep them in sync.** Recipe-level / dev-loop
changes also need updates in [HACKING.md](HACKING.md). Whenever you do a
major change or refactor (new DBus method, new config key, new backend,
new build/test step, renamed/moved components, anything that breaks an
existing how-to), update **both**:

* AGENTS.md (this file) — architecture, contracts, layering rules,
  changelog line.
* HACKING.md — affected recipe, debugging tip, dev-loop step, or PR
  checklist item.

* **2026-06-06** — Initial draft. Documents IdleManager, client cadence
  hints, config file, layering rules, and a test plan.
* **2026-06-06** — Connections iface filter moved off the inline filter bar
  to the main toolbar (`QToolButton` "All interfaces ▾") and the
  `View → "Show connections on"` submenu (both share one `QMenu` owned by
  the window). Added `qiftop -i <iface>` (iftop-style) — repeatable, sets
  the filter transiently (via new `Settings::setConnectionVisibleIfacesTransient`)
  and jumps to the Connections tab.
* **2026-06-06** — Added HACKING.md (recipe/cookbook companion). This file
  now points to it as the source of truth for build/run/debug recipes.
* **2026-06-06** — Toolbar iface filter is now context-sensitive: the
  `QToolButton` is hidden when the Connections tab is not active, and the
  `View → "Show connections on"` submenu action is disabled (grayed) in
  the same state. Toggled in `MainWindow::updateConnIfaceFilterVisibility()`
  on `QTabWidget::currentChanged`. Button popup mode switched to
  `MenuButtonPopup` (proper button face + dropdown arrow).
* **2026-06-06** — `.deb` packages are now regenerated automatically on
  every build via `add_custom_command(TARGET qiftop-agent POST_BUILD ...)`.
  Cache var: `QIFTOP_AUTO_PACKAGE` (default `ON`). Avoid the obvious
  alternative — `add_custom_target(... ALL)` with file-level DEPENDS —
  it causes an infinite reconfigure loop (cpack touches install manifests
  in the build dir → cmake reconfigure → custom target re-runs → loop).
* **2026-06-06** — Closed-connection retention window is now user-configurable
  via Preferences → Monitoring → "Keep closed connections for:" (range
  0…600 s, default bumped 5 s → **15 s**, `0` = prune immediately).
  Persisted as `connections/staleRetentionSecs`. Plumbed through
  `Settings::connectionStaleRetentionSecs()` →
  `ConnectionModel::setStaleRetentionMs()`.
* **2026-06-06** — UDP aggregation + direction inference + split alias toggle.
  - New client-side `Connection::Direction` enum (Unknown/Outbound/Inbound),
    computed in `ConnectionModel::updateConnections` from
    `/proc/sys/net/ipv4/ip_local_port_range` (the side whose port lands in
    the ephemeral range is the initiator). DBus wire format unchanged.
  - `Connection::key()` now includes direction so inbound/outbound
    aggregated rows for the same peer don't collide.
  - UDP flows are coalesced by peer when
    `Settings::udpAggregateByPeer` is on (default): outbound rows mask
    `local.port → 0`, inbound rows mask `remote.port → 0`; sums are
    accumulated; port `0` renders as `*`. Direction::Unknown UDP flows
    pass through individually. Tames DNS-burst noise.
  - Separate UDP stale-retention budget
    (`Settings::connectionStaleRetentionSecsUdp`, default **60 s**); TCP/
    other still use `connectionStaleRetentionSecs` (default 15 s). Branch
    happens in the prune loop on `Row::current.proto`.
  - Split "alias own addresses as localhost" into two policies: loopback
    (127.0.0.1/::1) is **always** aliased when DNS resolution is on;
    iface IPs are aliased only when
    `Settings::resolveIfaceAddrsAsLocalhost` is on (default true).
    `ConnectionModel` now keeps `m_loopbackAddrs` separate from
    `m_localAddrs`; `setLocalAddresses` strips loopbacks before storing.
  - New Preferences UI: UDP retention spinbox + "Aggregate UDP flows by
    peer" checkbox in Monitoring; "Render own interface addresses as
    localhost" checkbox in DNS group. Wired in
    `MainWindow::applySettingsToUi()`.
* **2026-06-06** — Connections display: proto label gains v4/v6 suffix
  (`TCPv4`, `UDPv6`, …) when DNS resolution is on — addresses become
  hostnames so the family is no longer visually obvious. ICMP rows are
  left alone (the proto name already carries the family). Implemented
  in `ConnectionModel::protoLabel`.
* **2026-06-06** — Right-click context menu on a connection row:
  "Copy source", "Copy destination", "Copy entire connection line".
  Source/destination follow `Connection::direction` (Outbound→local is
  source; Inbound→remote is source; Unknown→default to local→remote).
  Copy semantics differ from the on-screen text in one place: a
  non-loopback iface address shown as "localhost" is copied as its
  numeric IP, since "localhost" is meaningless when pasted off-host.
  Loopback (127.0.0.1/::1) still copies as "localhost" when DNS is on.
  Helpers: `ConnectionModel::{copyTextForEndpoint,copyTextForFlow,
  endpointCopyText}`; wired via `MainWindow::showConnectionContextMenu`.
* **2026-06-06** — **Bug fix:** display would stall after ~30–60 s because
  the GUI client subscribed to `StatsChanged` / `ConnectionsChanged`
  signals but never called any method again after the initial snapshot.
  The agent's `IdleManager` resets only on incoming method calls
  (`noteActivity()`), so with no calls coming in the agent progressively
  slowed (slow1 @ 30 s, slow2 @ 45 s) and paused polling entirely at
  `slow2WindowMs` (60 s). Fix: client now sends
  `SetDesiredIntervalMs(pollIntervalMs)` on start and re-asserts it on a
  4 s heartbeat (well under the 10 s `hintTtlMs` AND the 30 s
  `slow1WindowMs`). New virtual `setDesiredIntervalMs(int)` on
  `NetworkMonitor` / `ConnectionMonitor`; local backends ignore it,
  `DBus{Network,Connection}Monitor` translate it to an async
  `SetDesiredIntervalMs` DBus call. Heartbeat lives in
  `MainWindow::applySettingsToUi` so changes to the user's poll interval
  immediately repropagate.
* **2026-06-06** — **Bug fix:** IPv6 connections never appeared in the
  Connections tab. Root cause: `ConntrackMonitor::poll()` issued a single
  `nfct_query(h, NFCT_Q_DUMP, &family)` with `family = AF_UNSPEC`.
  Despite the documented "all families" semantics, many
  libnetfilter_conntrack / kernel combinations only return AF_INET
  entries for an AF_UNSPEC dump. Fix: issue two explicit dumps,
  `AF_INET` and `AF_INET6`, accumulating callbacks into the same flow
  list. A per-family failure no longer aborts the other dump.
* **2026-06-06** — Flow column rendering: (1) the `[PROTO]` tag is now
  always painted in upright (non-italic) type even when the row is
  stale, so the protocol label has one consistent typographic
  signature; (2) new setting `display/colorCodeConnectionFlow` (default
  on) tints the source endpoint in a cool color and the destination in
  a warm color (theme-aware: brighter on dark, deeper on light); proto
  tag + arrow remain muted in either mode. Source/destination follow
  `Connection::direction` (Outbound→local is source; Inbound→remote is
  source; Unknown→local→remote). Wired via new
  `ConnectionFlowDelegate::setColorCodeEnabled` and a Preferences →
  Display checkbox. New `ConnectionModel::DirectionRole` exposes the
  direction enum to the delegate.
* **2026-06-06** — Auto-enable per-family conntrack so IPv6 flows show
  up. The kernel only enables nf_conntrack on a given address family
  once *some* nftables/iptables rule references `ct` for that family.
  Hosts that have IPv4 NAT but no IPv6 ct references (common on
  desktops) end up with an empty IPv6 conntrack table, and qiftop's
  reader sees nothing. Fix: ship a tiny inert `inet qiftop` nftables
  shim (`dist/nftables/qiftop-conntrack.nft`, installed at
  `/usr/share/qiftop/`) and have the systemd unit load it via
  `ExecStartPre=-/usr/sbin/nft -f …` and remove it again via
  `ExecStopPost=-/usr/sbin/nft delete table inet qiftop`. The rule is
  pure-read (`ct state new counter` with `policy accept`) — its only
  purpose is to make the kernel wire up conntrack for both families.
  `nftables` added as `Recommends` on the agent .deb. The leading `-`
  on the systemd commands makes load/teardown best-effort: agent still
  starts on hosts without `nft` installed (the only consequence is no
  v6 visibility there).
* **2026-06-06** — Preferences is now a tabbed dialog (`QTabWidget`)
  with Monitoring / Display / DNS / Tray pages. Added two protocol
  toggles in Display: `display/showTcp` and `display/showUdp`
  (default both on); implemented as `ConnectionFilterProxy::
  {setShowTcp,setShowUdp}` keyed off new `ConnectionModel::ProtoRole`.
  Added direction-based row tint: `display/tintRowByDirection`
  (default off). When enabled, the model paints a faint green
  background on outbound rows and faint red on inbound rows
  (Wireshark-style); Unknown direction = no tint. Theme-aware
  (desaturated overlays on dark themes). The tint checkbox is grayed
  out in Preferences when color-coding is off (live, via QCheckBox
  `toggled`); `applySettingsToUi` also forces tint off when color-
  coding is off so the two flags can't drift out of sync. Wired
  through `ConnectionModel::setTintRowByDirection` which emits a
  `BackgroundRole`-only `dataChanged` so only the background repaints.
* **2026-06-06** — Added adaptive per-connection throughput gauge.
  New settings (Display tab): `display/throughputGaugeEnabled`
  (default off), `display/throughputMaxMode` (Windowed | Cumulative
  Average; default Windowed), `display/throughputWindowSecs`
  (default 30). New columns `RxMax`/`TxMax` on the Connections model
  (appended at the end so existing column indices stay stable);
  hidden by default and shown only when the gauge is enabled. New
  model roles `GaugeFractionRole` and `GaugeDarkColorRole`; new
  helpers `rxReference`/`txReference` compute the "max" baseline
  from either the sliding window of rate samples or a per-row
  running sum/count. Row tint is rendered a little fainter when the
  gauge is on so the darker filled portion reads. New
  `RowGaugeDelegate` paints the row-spanning gauge bar by computing
  the boundary in row-local coordinates from the horizontal header's
  section geometry (so the gauge looks continuous across cell
  boundaries). `ConnectionFlowDelegate` now inherits from
  `RowGaugeDelegate` and reuses its `paintGaugeBackground` before
  the rich-text content. The default item delegate on the
  connections table is now `RowGaugeDelegate`, with
  `ConnectionFlowDelegate` overriding only the Flow column.
  Right-click "Show Menu Bar" / "Show Toolbar" toggles and a
  "Preferences…" shortcut added to both tab context menus via the
  shared `MainWindow::appendViewToggleSection` helper.
* **2026-06-06** — Added per-connection rate smoothing. New setting
  `display/rateSmoothingSecs` (default 0 = off, range 0..60) is the
  EMA time constant τ in seconds. Applied in
  `ConnectionModel::updateConnections` as
  `α = Δt/(τ+Δt); rate ← α·raw + (1−α)·prev`, so smoothing stays
  physically consistent across varying poll intervals. Smoothed
  rates are stored in `Row::rxRate/txRate` directly, so the
  throughput gauge, Max RX/TX columns, sliding-window samples and
  the CMA all see the smoothed values consistently. New "Smooth
  rates over:" spinbox in the Display tab (special-value "Off" at
  0). Wired through `setRateSmoothingMs(int)` on the model from
  `MainWindow::applySettingsToUi`.
* **2026-06-06** — Added optional filter-summary suffix to the
  window title. New setting `display/showStatusInTitle` (default
  off); checkbox in the Display tab. When on,
  `MainWindow::updateWindowTitle()` builds a compact " · "-joined
  status string after the base title: poll cadence ("1s"/"500ms"),
  iface filter (when not "all"; "" → "—"), protocol-family overrides
  (TCP-only/UDP-only/"no protocols"), "IPv4-only" when IPv6 is off,
  and "no linger" when both stale-retention windows are zero.
  Called from `applySettingsToUi` and from
  `applyConnIfaceFilterToProxy` so the title stays accurate when
  the iface filter is toggled from the toolbar/View menu.
* **2026-06-07** — Direction inference fallback for well-known-on-
  both-ends flows (mDNS 5353/5353, DHCP 67/68, NTP 123/123). The
  ephemeral-port heuristic could only classify when exactly one
  side was in the local ephemeral range; for the cases above both
  sides sit below 1024, so direction fell through to Unknown and
  the row rendered untinted. Added a second-stage check in
  `qiftop::heuristics::inferDirection`: if exactly one endpoint's
  address belongs to the local set (interface IPs ∪ loopback), that
  side is the initiator. Forwarded/transit flows where *neither*
  end is local still resolve to Unknown by design.
* **2026-06-07** — Introduced Qt6::Test unit tests under `tests/`,
  gated by `option(QIFTOP_BUILD_TESTS ON)`. Pure-logic helpers
  extracted from UI into header-only `src/ui/ConnectionHeuristics.h`
  (`inferDirection`, `isForwardedFlow`, `emaUpdate`,
  `emaUpdateAsym`, `easeOutCubic`) so they can be exercised without
  spinning up a model. One executable per .cpp; `qiftop_add_test()`
  helper in `tests/CMakeLists.txt`. Tests that need Settings or
  Autostart link the source TUs directly (avoid bundling the whole
  GUI binary into a test). See HACKING.md §5.5–5.7 for conventions.
* **2026-06-07** — Refactored per-connection rate smoothing into a
  three-stage pipeline so the gauge animates between polls:
  1. **Raw** (`Row::rxRaw/txRaw`) drives the throughput reference
     (Max columns and gauge denominator) — never smoothed, so the
     gauge ceiling reflects actual observed peaks.
  2. **Target** = symmetric EMA of raw rates at τ
     (`Settings::rateSmoothingMs`).
  3. **Display** = easeOutCubic tween from `animFrom → target` over
     `pollMs` (rises) or `max(100ms, pollMs/3)` (falls); advanced
     by `ConnectionModel::advanceSmoothing()` which `MainWindow`
     drives via a sub-poll `m_smoothingTick` `QTimer` at
     `max(100ms, pollMs/4)`. Stopped when smoothing is off so we
     don't paint at 4 Hz for no reason.
* **2026-06-07** — XDG Autostart support. New `src/util/Autostart.{h,cpp}`
  (namespace `qiftop::autostart`) manages
  `$XDG_CONFIG_HOME/autostart/qiftop.desktop`. New
  `Settings::startOnLogin()/setStartOnLogin()` — filesystem is the
  source of truth (no QSettings key), so the spec-compliant
  `Hidden=true` / `X-GNOME-Autostart-enabled=false` flags from
  other autostart managers are honored. New `--tray` CLI flag in
  `main.cpp` suppresses the initial `show()`; the .desktop entry
  written by the autostart helper appends `--tray` to `Exec=`.
  "Start on login (silently into tray)" checkbox on Tray tab.
* **2026-06-07** — Menu refactor. View → "Main" (the toolbar's
  default `toggleViewAction()` title) renamed to "Show Toolbar".
  Added "Show Menu Bar" (custom checkable QAction, kept in sync
  via `aboutToShow` since the menu bar has no built-in toggle).
  File menu gained "Close to tray" (Ctrl+W), visible only when
  `closeToTray && tray->isAvailable()` — refreshed in
  `applySettingsToUi()` and on tray-retry success. Note the
  Ctrl+Q gotcha logged in HACKING.md: `QKeySequence::Quit ==
  Ctrl+Q` on Linux, so the Quit action uses
  `Qt::ApplicationShortcut` to dodge the ambiguous-shortcut
  warning when hiding the menu bar.
* **2026-06-07** — Tier-1 filter expression mini-language for the
  Connections view. New `src/util/ConnectionFilter.{h,cpp}` — pure
  parser/AST/evaluator with no Qt model/view dep. Grammar (see
  header):
    - Boolean: `and`/`&&`, `or`/`||`, `not`/`!`, parens.
    - String fields: `proto`, `src`, `dst`, `host` (matches either
      end including resolved hostname), `iface`, `family` (`v4`/`v6`),
      `direction` (`in`/`out`/`unknown`).
    - Numeric fields: `sport`, `dport`, `port` (either end),
      `bytes_in/out/total`, `pkts_in/out/total`, `rate_in/out/total`.
    - Ops: `:` (substring/eq), `=`, `!=`, `~` (regex,
      case-insensitive), `<`, `<=`, `>`, `>=`.
    - Numeric byte suffixes: `K/M/G/T` (×1000) and `Ki/Mi/Gi/Ti`
      (×1024) — `1Mi` == 1048576.
  Wire-up:
    - New model roles `ConnectionRole/RxRateRole/TxRateRole/
      HostnameLocalRole/HostnameRemoteRole` on `ConnectionModel`.
    - `ConnectionFilterProxy::setFilterExpression(QString) -> QString`
      returns parse error (empty on success); cheapest filters
      (v6/tcp/udp/iface) run before expression eval.
    - Toolbar `QLineEdit` next to the iface filter, visible only on
      the Connections tab. 200ms debounce on `textChanged`. Invalid
      input tints the line edit pink and surfaces the parser error
      in its tooltip; valid expressions persist as
      `connections/filterExpr`.
    - Trailing "?" `QAction` on the line edit (via
      `QLineEdit::addAction(..., TrailingPosition)`) pops a syntax
      cheat-sheet `QToolTip` anchored to the edit
      (`qiftop::filter::helpHtml()`). The line edit's normal tooltip
      also carries the cheat sheet for hover discovery.
  Rejected libjq for live filtering: the DBus wire format is native
  Qt marshalling (`a(yysqysqtttts)`), not JSON, so adding jq would
  mean serializing every row on every poll just to filter. Kept jq
  on the table as a Tier-2 option for export snapshots later.
* **2026-06-07** — Toolbar polish on the Connections tab. The iface
  dropdown and the filter group are now separated by a
  `QToolBar::addSeparator()` plus an expanding-spacer
  `QWidget` (`QSizePolicy::Expanding`), so the Filter group is
  right-anchored to the toolbar edge but collapses gracefully when the
  window is too narrow (zero minimum width on the spacer). The filter
  group itself is a `QWidget` container with
  `QSizePolicy::Maximum` horizontally — without this, the toolbar
  would stretch the wrapper while the inner `QLineEdit`'s
  `maximumWidth=440` cap prevented absorption, dropping the slack into
  the layout spacing between "Filter:" and the edit (visible 1000+px
  gap on wide windows). Added an explicit "Filter:" `QLabel` prefix
  and 6px inter-widget spacing. Separator + spacer visibility track
  the Connections tab via `updateConnIfaceFilterVisibility()`.
* **2026-06-07** — Replaced the fleeting `QToolTip::showText` on the
  filter "?" badge with a persistent `Qt::Popup` `QFrame`
  (`MainWindow::m_filterHelpPopup`, built lazily and reused) carrying
  a rich-text `QLabel`. `QToolTip` has its own dismiss timer plus
  triggers on focus changes and mouse leaves, so it's wrong for
  click-to-summon help where users hover over the text to read it.
  `Qt::Popup` stays open until the user clicks outside or hits
  Escape. Anchored under the line edit's right edge so it doesn't
  cover what the user is typing.
* **2026-06-07** — GitHub Actions CI + release automation.
  - `.github/workflows/ci.yml` — push/PR builds against a matrix of
    ubuntu-22.04 + ubuntu-24.04 × Debug + Release. Ninja generator,
    `QIFTOP_BUILD_TESTS=ON`, `QIFTOP_AUTO_PACKAGE=OFF` (the POST_BUILD
    cpack hook is only useful for local dev). Tests run under
    `dbus-run-session` with `QT_QPA_PLATFORM=offscreen` and a writable
    `HOME` redirected to `$RUNNER_TEMP/home` so QSettings /
    Autostart tests don't trample the runner user.
  - `.github/workflows/release.yml` — triggered on `v*` tag push.
    Verifies `project(qiftop VERSION ...)` base matches the tag (`v0.2-rc1`
    → `0.2`), runs the full test suite as a smoke check, packages
    `cpack -G DEB`, computes `SHA256SUMS`, and publishes a GitHub
    Release via `softprops/action-gh-release@v2` with auto-generated
    notes (`generate_release_notes: true`). Prerelease flag is
    auto-set when the tag contains `-`. Permissions scoped to
    `contents: write` only.
  - `.github/release.yml` — category grouping config for the
    auto-generated notes (Breaking / Features / Fixes / Packaging /
    Docs / Maintenance / Other), with `dependencies` label and
    `dependabot`/`github-actions` author commits excluded.
  - HACKING.md §9 rewritten around the automated path; manual cpack
    fallback documented for offline release cuts. README.md gained a
    CI status badge.
