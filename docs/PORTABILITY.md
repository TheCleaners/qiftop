# Portability — multi-OS / multi-datapath roadmap

> Status: **exploratory**. qiftop is primarily a Linux product, but the
> tree now has a reusable Widgets-free `libqiftop`, the Qt Widgets GUI
> (`qiftop`), and the ncurses terminal frontend (`nqiftop`) — and, as of the
> BSD experiment, a real (interface-only) `backend/bsd` that builds and runs
> the GUI/TUI/library + monitoring plugin on NetBSD 11 (and, by construction,
> the other BSDs). This document records the current portability boundary and
> the backend-abstraction rules to preserve for future ports.

---

## 1. Current portability boundary

| Layer | Today | Portability |
|-------|-------|-------------|
| `libqiftop` core (`aggregate/`, `dbus/`, `config/`, `dns/`, most `util/`, abstract `backend/`) | Qt6::Core/Network/DBus shared library; no Qt Widgets | 🟡 Source is intended to stay platform-clean, but the top-level build currently requires a concrete backend and errors out off Linux. |
| `ui/` + `qiftop_ui` | Qt 6 Widgets GUI, models, delegates, tray, and GUI-only elevation helpers | 🟡 Widgets code is portable Qt; bundled self-elevation / handoff helpers are Linux/POSIX desktop glue. |
| `tui/` / `nqiftop` | ncursesw frontend built on `libqiftop` | 🟡 Headless and Widgets-free, but built only on Linux today; raw curses and terminal syscalls are isolated to `src/tui/`. |
| `dns/` | `QHostInfo`-based async resolver | ✅ Portable. |
| `config/` | `QSettings` | ✅ Portable. |
| `aggregate/` | Plain `QObject` interface/connection aggregators | ✅ Portable and shared by GUI/TUI/library consumers. |
| `util/Units`, `util/ConnectionFilter`, `util/Exporter`, `util/Autostart` | Pure C++/Qt helpers | ✅ / 🟡 Mostly portable; `Autostart` is XDG-desktop specific. |
| `util/Handoff*`, `util/PrivilegeEscalator` | GUI fallback elevation IPC and helper launching | 🔴 Linux/POSIX desktop-specific; not part of the installed `libqiftop` headers. |
| `backend/` interfaces | `NetworkMonitor`, `ConnectionMonitor`, `ProcessResolver`, DTO-adjacent value types | ✅ Abstract, no platform headers. |
| `backend/null/` | No-op `ProcessResolver` fallback | ✅ Portable. |
| `backend/dbus/` | Client-side DBus proxy monitors | 🟡 Portable wherever Qt DBus and a suitable bus are available. |
| `backend/PlatformInfo` | Local address / uid-name / group helpers with `Q_OS_*` guards | 🟡 Compiles with fallbacks; richer data on Unix/Linux. |
| `backend/linux/` | libnl-3, libnetfilter_conntrack, sock_diag, cgroup and netns attribution | 🔴 Linux-only. |
| `backend/bsd/` | getifaddrs(3) AF_LINK per-interface counters (NetBSD/FreeBSD/OpenBSD/DragonFly/macOS); per-flow capture is a stub | 🟡 Interface stats work today on NetBSD 11; per-flow needs a pf/npf/BPF datapath. |
| `agent/` | DBus daemon wrapping the abstract monitors/resolver | 🟡 Service logic is mostly Qt Core/DBus; production data sources and install model are Linux. |
| `dist/systemd/`, `dist/dbus/`, `dist/debian/`, RPM/Pages repo rules | systemd, system DBus policy, Debian/Fedora packaging | 🔴 Linux-distro-specific. |

Legend: ✅ portable / 🟡 partly portable / 🔴 OS-specific.

The backend shape is still the key abstraction: `NetworkMonitor` emits
per-interface snapshots, `ConnectionMonitor` emits per-flow snapshots and
lazy process-detail replies, and `ProcessResolver` enriches flows with
process/container metadata. Kernel-facing implementations live under
`backend/<os>/` (`backend/linux/` today); client-side transport proxies live
under `backend/dbus/`; the universal resolver fallback lives under
`backend/null/`.

Layering rule to preserve: code in `dbus/`, `dns/`, `config/`, `aggregate/`,
the abstract `backend/` headers, and the library-safe subset of `util/` must
remain free of Qt Widgets and platform headers. UI code may use Qt Widgets;
raw platform/terminal glue should stay in narrow leaf files (`backend/linux/`,
`backend/PlatformInfo.cpp`, `src/tui/Screen.cpp` / `main.cpp`, or the current
GUI elevation helpers) rather than leaking into public headers.

---

## 2. Target platforms — data-source survey

These are **not support claims**; they are the likely APIs a future backend
would need.

### 2.1 Linux (current)

| Need | API |
|------|-----|
| Per-interface counters | rtnetlink via libnl-3 (`rtnl_link_*`) |
| Per-flow counters | libnetfilter_conntrack (`NFCT_Q_DUMP`; qiftop issues separate IPv4 and IPv6 dumps) |
| Local addresses | `getifaddrs()` via `backend/PlatformInfo` |
| Ephemeral port range | `/proc/sys/net/ipv4/ip_local_port_range` |
| Socket → PID | `NETLINK_SOCK_DIAG` plus `/proc/<pid>/fd` walk |
| PID → cgroup/container | `/proc/<pid>/cgroup` via `CgroupClassifier` |
| Cross-netns flows | `setns(CLONE_NEWNET)` plus sock_diag inside each container netns (`NetnsScanner`) |
| Privilege model | root `qiftop-agent` with `CAP_NET_ADMIN`, `CAP_SYS_PTRACE`, `CAP_DAC_READ_SEARCH`, `CAP_SYS_ADMIN`; system DBus; `netdev` policy gate; systemd sandbox |
| Clients | Qt Widgets GUI (`qiftop`), ncurses TUI (`nqiftop`), and `libqiftop` consumers via DBus proxies |

### 2.2 FreeBSD / OpenBSD / NetBSD

| Need | API | Status |
|------|-----|--------|
| Per-interface counters | `getifaddrs()` AF_LINK `struct if_data` (or `sysctl net.link.generic.ifdata.*`) | ✅ Implemented in `backend/bsd` (`BsdNetworkWorker`); verified on NetBSD 11. |
| Per-flow counters | `pfctl -ss` parse, **libpfctl** (FreeBSD 13+), NetBSD `npf`, or BPF + userspace accounting | 🔴 Stubbed (`BsdConnectionMonitor` emits `accountingUnavailable`). |
| Local addresses | `getifaddrs()` | ✅ Folded into the interface snapshot (AF_INET/AF_INET6 CIDRs). |
| Ephemeral port range | `sysctl net.inet.ip.portrange.first/last` | ⬜ Not needed until per-flow direction inference lands. |
| Socket → PID | `sysctl net.inet.tcp.pcblist` plus `kvm_*` / `procstat` | ⬜ Future (needs per-flow first). |
| Container attribution | Jails via `jail_get(2)`; OCI runtime attribution is platform-specific | ⬜ Future. |
| Privilege model | Setuid/helper daemon or capsicum-sandboxed service; DBus not assumed | ⬜ Agent is Linux-only today; BSD runs the in-process backend. |

What works today (NetBSD 11, `pkgsrc` qt6-qtbase + base curses): `libqiftop.so`,
the `qiftop` GUI, the `nqiftop` TUI, and the `check_qiftop` monitoring plugin
all build and run. The Interfaces view shows live per-interface rates, totals,
MTU, and oper-state from `getifaddrs(3)`; the Connections view is empty (the
per-flow stub reports accounting unavailable). The `backend/bsd` code is shared
across the whole BSD family — only the future per-flow datapath (pf on
FreeBSD/OpenBSD, npf on NetBSD) diverges. The privileged `qiftop-agent` and its
attribution layer remain Linux-only.

Verdict: interface monitoring is **done**; per-flow accounting is the next
(moderate) lift, larger still for a DBus-free transport and rich attribution.

### 2.3 macOS / Darwin

| Need | API |
|------|-----|
| Per-interface counters | `sysctl net.link.iflist` / `if_data`, or another Network framework source |
| Per-flow counters | No conntrack equivalent. Options are private `NetworkStatistics`, Network Extension, BPF + userspace flow tracking, or disabling the Connections view. |
| Local addresses | `getifaddrs()` |
| Ephemeral port range | `sysctl net.inet.ip.portrange.first/last` |
| Socket → PID | `proc_listpids` + `proc_pidfdinfo` (libproc) |
| Container attribution | None by default; Docker Desktop/VM attribution would be indirect |
| Privilege model | `SMAppService` / privileged helper, signed bundle, no DBus by default |

Verdict: **hard**. The main blocker is trustworthy per-flow byte/packet
accounting without a Linux-style conntrack table.

### 2.4 Windows

| Need | API |
|------|-----|
| Per-interface counters | `GetIfTable2` / `GetIfEntry2` (`iphlpapi`) |
| Per-flow counters | `GetExtendedTcpTable` / `GetExtendedUdpTable` snapshots, ETW Kernel-Network provider for deltas, or a driver such as WinDivert |
| Local addresses | `GetAdaptersAddresses` |
| Ephemeral port range | Modern TCP dynamic port APIs or registry fallback |
| Socket → PID | owner-PID TCP/UDP tables |
| Container attribution | Windows/Hyper-V container metadata or WSL2 delegation |
| Privilege model | Windows service / UAC; no DBus |

Verdict: **moderate** for interface and PID-owned socket snapshots; harder
for real-time byte accounting and packaging.

---

## 3. Architectural decisions to preserve

1. **Backends stay behind abstract `QObject` interfaces.**
   `NetworkMonitor`, `ConnectionMonitor`, and `ProcessResolver` headers must
   stay platform-clean. Concrete capture/enrichment code goes in
   `backend/<os>/`; transport proxies go in `backend/dbus/`; no-op fallbacks
   go in `backend/null/`.
2. **Shared library code stays Widgets-free.** `libqiftop` is Qt
   Core/Network/DBus only. Anything that needs `QWidget`, item views,
   delegates, tray UI, ncurses, or elevation UI belongs in the GUI/TUI layer,
   not in the installed library headers.
3. **DTOs use stable external encodings.** DBus DTO fields use IANA protocol
   numbers, RFC/kernel values, ifindex, and explicit fallback values rather
   than private enum ordinals.
4. **Capability tokens beat version checks.** Optional behaviour is advertised
   by append-only tokens (`process-attribution-wire`, `container-chain-wire`,
   `tcp-state`, `on-demand-process-details`, …). Clients must treat missing
   tokens as "feature absent".
5. **Compile-time options gate compile-in; runtime probes gate use.** The
   Linux attribution toggles (`QIFTOP_ENABLE_PROCESS_ATTRIBUTION`,
   `QIFTOP_ENABLE_CONTAINER_ATTRIBUTION`, `QIFTOP_ENABLE_NETNS_SCAN`) compile
   sources in or out; resolver `initialize()` and `capabilities()` decide what
   is actually available at runtime.
6. **Adding a platform requires a real backend clause.** The root
   `CMakeLists.txt` detects a platform *family* into `QIFTOP_PLATFORM`
   (`linux`, `bsd`, or empty → `FATAL_ERROR`). A port adds
   `src/backend/<family>/`, links the static `backend_<family>` target into
   `qiftoplib`, defines `BACKEND_<FAMILY>`, sets `QIFTOP_BACKEND_TARGET` /
   `QIFTOP_BACKEND_DEFINE`, and adds the matching `#ifdef BACKEND_<FAMILY>`
   construction branch in `src/main.cpp` and `src/tui/main.cpp`. The `bsd`
   family is the worked example: one `getifaddrs(3)` backend shared by every
   BSD, with `__has_include`-based curses selection (`src/tui/Curses.h`) and a
   `getpeereid(3)` fallback for the handoff peer-credential check. The
   `qiftop-agent` target is gated to `QIFTOP_PLATFORM STREQUAL "linux"`.

---

## 4. The main roadblocks

### 4.1 DBus transport

The shipped agent IPC is DBus. That works well on Linux system/session buses,
but macOS and Windows do not have a native system bus and BSD availability is
not guaranteed. Because clients talk to `NetworkMonitor` / `ConnectionMonitor`
objects, a future non-DBus transport could provide the same abstract monitor
shape, but it would need a new framing/authentication design and equivalent
capability discovery.

### 4.2 Privilege model

The current privileged path assumes a Linux root daemon (preferred) or Linux
desktop elevation helpers (GUI fallback). Other OSes need native privilege
install/activation stories: launchd helpers on macOS, a Windows service/UAC,
or a BSD daemon/setuid-helper model. `nqiftop` has no GUI self-elevation path;
its in-process fallback simply requires the process to have the needed rights.

### 4.3 Per-flow accounting on macOS and some BSD setups

Linux conntrack gives qiftop cheap, kernel-maintained byte/packet counters per
flow. macOS has no direct equivalent, and BSD options depend heavily on pf/BPF
availability and privilege. Any port may need to ship interface-only mode first
or omit the Connections view by leaving the relevant capability tokens absent.

### 4.4 Packaging and policy

Debian/Ubuntu and Fedora packaging now exist, but they are still Linux-specific
(systemd unit, system DBus policy, apt/dnf repositories, Linux capabilities).
macOS, Windows, and BSD ports would need new installer, service activation,
code-signing, and access-control policy work without changing the abstract
backend interfaces.

---

## 5. Concrete recommendations going forward

* Keep kernel/data-source code in `backend/<os>/` and keep public backend
  headers platform-clean. `backend/PlatformInfo.cpp` is the narrow exception
  for small host probes with guarded fallbacks.
* Do not add Qt Widgets, curses, or platform headers to `libqiftop` public
  headers (`aggregate/`, `dbus/`, `config/`, `dns/`, portable `util/`, and
  abstract `backend/`).
* If GUI/TUI platform glue needs OS headers, isolate it in a leaf translation
  unit and do not let it become a dependency of the shared library API.
* Add a capability token for every new optional agent/transport feature.
  Never gate client behaviour on string-comparing the agent `Version`.
* Follow the `ProcessResolver` pattern for future enrichment features:
  abstract interface, null fallback, `<os>/` implementation, factory/probe,
  bounded caches, and runtime capability tokens.
* When adding a new frontend, link `libqiftop` and keep rendering/toolkit code
  at the edge, like `qiftop_ui` for Widgets and `src/tui/Screen.cpp` for
  ncurses.

---

## 6. What's not planned for 0.2.x

* Full macOS / Windows support.
* **BSD per-flow accounting** (pf/npf/BPF) — the `backend/bsd` interface
  monitor exists and works, but the Connections view stays empty on BSD until
  a per-flow datapath is written. Interface-only BSD support is a byproduct of
  the portability experiment, not a shipped/packaged target yet.
* A non-DBus production transport.
* A macOS Network Extension / notarized collector.
* A Windows packet driver integration.
* A BSD `qiftop-agent` (privileged daemon + attribution); BSD runs the
  in-process backend only.
* Shipping in-process privileged capture to third-party `libqiftop`
  consumers; installed library consumers are expected to stream from the
  agent via the DBus proxies.
