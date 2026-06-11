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
| `backend/bsd/` | getifaddrs(3) AF_LINK per-interface counters + libpcap/BPF per-flow capture (NetBSD/FreeBSD/OpenBSD/DragonFly/macOS) | 🟡 Interface stats + per-flow rates work on NetBSD 11; no process/container attribution. |
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
| Per-flow counters | libpcap/BPF + userspace 5-tuple flow table (the iftop model). Alternatives: `pfctl -ss`, **libpfctl** (FreeBSD 13+), NetBSD `npf` | ✅ Implemented in `backend/bsd` (`BsdConnectionWorker`); verified on NetBSD 11. |
| Local addresses | `getifaddrs()` | ✅ Folded into the interface snapshot (AF_INET/AF_INET6 CIDRs). |
| Ephemeral port range | `sysctl net.inet.ip.portrange.first/last` | ✅ Read by `BsdConnectionWorker` for direction inference. |
| Socket → PID | `KERN_FILE2` ⋈ `net.inet.*.pcblist` ⋈ `KERN_PROC2` (the sockstat join; pure sysctl, no kvm) | ✅ Implemented in `backend/bsd` (`BsdSocketResolver`); flows carry comm/pid/uid on NetBSD. |
| Container attribution | Jails via `jail_get(2)`; OCI runtime attribution is platform-specific | ⬜ Future (jails not yet wired). |
| Privilege model | Setuid/helper daemon or capsicum-sandboxed service; DBus not assumed | ⬜ Agent is Linux-only today; BSD runs the in-process backend (capture needs root for `/dev/bpf`). |

What works today (NetBSD 11, `pkgsrc` qt6-qtbase + base curses + base libpcap):
`libqiftop.so`, the `qiftop` GUI, the `nqiftop` TUI, and the `check_qiftop`
monitoring plugin all build and run. The Interfaces view shows live
per-interface rates, totals, MTU, and oper-state from `getifaddrs(3)`; the
Connections view shows live per-flow TCP/UDP rates, totals, SYN-observed
direction, and **per-flow process attribution** (comm/pid/uid, with
group-by-process) captured via libpcap/BPF + a pure-sysctl socket→PID join
(run with elevated privileges for `/dev/bpf` access). The `backend/bsd` code
is shared across the whole BSD family — only the sysctl struct layouts in the
process resolver are currently NetBSD-specific (other BSDs build with
attribution stubbed). Remaining gaps: container/jail attribution and a
privileged agent. The `qiftop-agent` remains Linux-only; on BSD the
in-process backend is the natural, DBus-free path.

Verdict: interface, per-flow, **and** process attribution are working on BSD;
the next lifts are container/jail attribution and an optional privileged-agent
/ non-DBus transport.

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
* **BSD container/jail attribution** — interface, per-flow (libpcap/BPF), and
  per-process attribution all work on NetBSD, but flows carry no container/jail
  scope yet (`jail_get(2)` is unwired). BSD support is a byproduct of the
  portability experiment, not a shipped/packaged target yet. The process
  resolver's sysctl struct layouts are currently NetBSD-specific; FreeBSD/
  OpenBSD build with attribution stubbed.
* A non-DBus production transport.
* A macOS Network Extension / notarized collector.
* A Windows packet driver integration.
* A BSD `qiftop-agent` (privileged daemon + attribution); BSD runs the
  in-process backend only (capture needs root for `/dev/bpf`).
* Shipping in-process privileged capture to third-party `libqiftop`
  consumers; installed library consumers are expected to stream from the
  agent via the DBus proxies.

---

## 7. BSD port — implementation notes & hard-won lessons

This section is the field guide for anyone extending the BSD backend (or
porting qiftop, or a similar Qt6/libpcap tool, to another BSD or macOS). It
records the things that cost trial-and-error so you don't repeat them. The
code lives in `src/backend/bsd/` and is shared across the BSD family; only the
process-attribution sysctl struct layouts are currently NetBSD-specific.

### 7.1 Build environment (NetBSD 11, validated)

* Toolchain: base GCC (12.x) is fine; `cmake`, `ninja`, `pkg-config` from
  pkgsrc. Non-login shells do **not** have pkgsrc/base sbin on `PATH` — export
  `PATH=/usr/pkg/bin:/usr/pkg/sbin:/usr/sbin:/sbin:$PATH` before building or
  running, or `sysctl`/`ldconfig`/etc. "command not found" will bite you.
* Qt6: `pkgsrc` `qt6-qtbase` installs under `/usr/pkg/qt6` with the
  `netbsd-g++` mkspec. CMake finds it without hints once `/usr/pkg` is on the
  prefix path. Widgets/Network/DBus/Test are all present.
* Curses: NetBSD **base** curses (`/usr/lib/libcurses`) is wide-capable but is
  NOT named `ncursesw`, so CMake's `find_package(Curses)` with
  `CURSES_NEED_WIDE` misses it. We drop `CURSES_NEED_WIDE` on the BSD family
  and use base curses (see `CMakeLists.txt`). Installing pkgsrc `ncurses` is
  unnecessary and actively unhelpful (its headers live under
  `/usr/pkg/include/ncurses/` and self-reference `<ncurses/ncurses_dll.h>`,
  which needs `-I/usr/pkg/include` too — easier to just use base curses).
* libpcap: in base (`/usr/lib/libpcap`, `pcap.h`). No pkgsrc dependency
  needed. `pcap-config --libs` exists.

### 7.2 CMake platform abstraction

`QIFTOP_PLATFORM` (`linux`|`bsd`|empty) is derived from `CMAKE_SYSTEM_NAME`
(`NetBSD|FreeBSD|OpenBSD|DragonFly` → `bsd`). It drives `add_subdirectory`,
the `BACKEND_<FAMILY>` compile define, and `QIFTOP_BACKEND_TARGET` /
`QIFTOP_BACKEND_DEFINE` (reused by `nqiftop`, which links a backend directly).
`qiftop-agent` is gated to `linux` (its attribution layer pulls in
`backend/linux/` headers). When you gate a target out, also guard its
`install(TARGETS ...)` and any `add_custom_command(TARGET ...)` — CMake errors
at configure time on a missing target, not just at build.

### 7.3 Interface counters — `getifaddrs(3)` AF_LINK

Portable across every BSD and macOS. Per-interface counters hang off the
`AF_LINK` entry's `ifa_data` as `struct if_data`: `ifi_ibytes/obytes`,
`ifi_ipackets/opackets`, `ifi_ierrors/oerrors`, `ifi_iqdrops` (there is no
output-drop counter — leave `txDropped` 0), `ifi_mtu`, `ifi_type` (IFT_*),
`ifi_link_state` (`LINK_STATE_{UNKNOWN,DOWN,UP}` → map to RFC 2863 oper-state).
Walk AF_INET/AF_INET6 entries separately for addresses. `IFT_*` constants vary
by OS — guard the less-common ones with `#ifdef`.

### 7.4 ncurses wide-character rendering (the garbled-frames trap)

Symptom: box-drawing frames render as garbage on BSD but fine on Linux.
Cause: the narrow `addstr()`/`mvaddstr()` family takes a `char*`; Linux
ncurses decodes the multibyte UTF-8 into cells, but BSD base curses places
each *byte* in its own cell. Fix: render through the **wide** API
(`mvaddwstr`, `wchar_t`). `QString::toWCharArray` emits UCS-4 where `wchar_t`
is 4 bytes (Linux/BSD), so the conversion is a one-liner. Two more
requirements:
* Request wide prototypes before including curses: define
  `_XOPEN_SOURCE_EXTENDED` and `NCURSES_WIDECHAR` (see `src/tui/Curses.h`,
  which also picks `<ncurses.h>` vs `<curses.h>` via `__has_include`).
* Wide output needs a UTF-8 `LC_CTYPE`. `setlocale(LC_ALL, "")` honours the
  environment, but over SSH that is often `C`/`POSIX` (ASCII), and then
  `wcrtomb` can't encode the glyphs. After `setlocale`, check
  `nl_langinfo(CODESET)` and fall back to `C.UTF-8`/`en_US.UTF-8` if it isn't
  UTF-8 (see `Screen::init`). This fix also helps marginal Linux locales.

### 7.5 Peer credentials — `getpeereid(3)` not `SO_PEERCRED`

Linux's `SO_PEERCRED` / `struct ucred` does not exist on the BSDs. Use
`getpeereid(fd, &euid, &egid)` (NetBSD/FreeBSD/OpenBSD/DragonFly/macOS) when
you only need the peer uid (as the handoff credential gate does). See the
`#if defined(__linux__)` split in `src/util/HandoffServer.cpp`.

### 7.6 Per-flow capture — libpcap/BPF (there is no conntrack)

This is the iftop model and the portable BSD/macOS datapath. Worker thread
opens one `pcap_open_live` handle per up interface (non-promiscuous, snaplen
~128 — enough for L2+IPv6+TCP headers), non-blocking, with a `QSocketNotifier`
on `pcap_get_selectable_fd` so capture is event-driven, not polled. Gotchas:

* **Link-layer header length is per-DLT and per-OS.** Switch on
  `pcap_datalink()`: `DLT_EN10MB` = 14 (handle one `0x8100` VLAN tag → 18),
  `DLT_NULL`/`DLT_LOOP` = 4-byte address-family header, `DLT_RAW` = 0. **Do
  not hardcode `DLT_RAW`'s numeric value** — it differs across BSDs (14 on
  NetBSD, 12 elsewhere); always use the macro. We sniff the IP version nibble
  after the header rather than trusting the AF word, which sidesteps
  `DLT_NULL` host/network byte-order ambiguity.
* **IPv6 extension headers are not chased.** We read `next header` and assume
  L4 at +40; if it isn't TCP/UDP we simply don't read ports. Good enough for a
  rate monitor; revisit if you need exact ports through ext-header chains.
* **Orientation & flow merging.** Compare src/dst against the local interface
  address set to decide tx vs rx and to merge both half-flows into one entry.
  Normalise both-local (loopback) and forwarded flows by a deterministic
  endpoint ordering (higher port = "local") so the two directions collapse to
  one key. The flow key (see `BsdFlowKey.h`) is shared with the resolver so a
  captured flow and its owning PCB hash identically.
* **Direction from the TCP SYN.** A SYN-without-ACK identifies the initiator:
  SYN-from-local ⇒ outbound, SYN-from-remote ⇒ inbound. This beats the
  ephemeral-port heuristic (which misfires when peers use different local port
  ranges — e.g. a Linux client's 33xxx port looks non-ephemeral to NetBSD's
  49152+ range) and is *more* accurate than qiftop's Linux in-process path
  (which leaves direction Unknown). Fall back to `inferDirection()` when no
  SYN was observed (capture started mid-connection). This logic is
  platform-neutral and is the prime candidate to offer as a Linux pcap capture
  path too.
* **Bounded caches.** Cap the flow table (we use 16384; drop *new* flows at
  the cap so existing rates stay correct, never clear) and the emitted
  snapshot (top-N by bytes, 4096) per AGENTS §8a rule 8. Prune by a last-seen
  TTL each tick.
* Capture needs read access to `/dev/bpf*` — root on the BSDs. Degrade to a
  one-shot `accountingUnavailable` signal when no handle opens.

### 7.7 Process attribution — the pure-sysctl socket→PID join

NetBSD maps sockets to PIDs **without kvm** — exactly what `sockstat(1)` does
(read its source: `usr.bin/sockstat/sockstat.c`). Three sysctls, joined:

1. `KERN_FILE2` / `KERN_FILE_BYPID` → `struct kinfo_file[]`: for each
   `DTYPE_SOCKET` fd, `ki_fdata` is the `struct socket *` kernel pointer and
   `ki_pid` is the owner. Use **BYPID** (not BYFILE) — BYFILE does not
   populate `ki_pid` per descriptor.
2. `net.inet.{tcp,udp}.pcblist` / `net.inet6.{tcp6,udp6}.pcblist` →
   `struct kinfo_pcb[]`: `ki_sockaddr` is the same `struct socket *` pointer
   (the join key), `ki_src`/`ki_dst` are the local/foreign sockaddrs.
3. `KERN_PROC2` / `KERN_PROC_ALL` → `struct kinfo_proc2[]`: `p_pid` → `p_comm`
   + `p_ruid`.

Join `pcb.ki_sockaddr == file.ki_fdata` to get pid, then pid → comm/uid. Build
both an exact 4-tuple map and a local 2-tuple fallback (for listeners /
unconnected UDP, mirroring the Linux sock_diag 2-tuple path). See
`BsdSocketResolver`. **Two traps that cost real time:**

* **The MIB needs trailing args appended after `sysctlnametomib`.** For
  `pcblist`: append `{ PCB_ALL, 0 /*all pids*/, sizeof(struct kinfo_pcb),
  INT_MAX /*count*/ }`. For `KERN_FILE2`/`KERN_PROC2`: the op
  (`KERN_FILE_BYPID` / `KERN_PROC_ALL`), an arg, `sizeof(elem)`, and a count.
  A bare `sysctlbyname("net.inet.tcp.pcblist", ...)` returns `EINVAL` — that's
  the symptom of the missing trailing elements. `PCB_ALL` is `0`, defined in
  `<sys/socket.h>`.
* **A sysctl size-probe (`oldp == NULL`) returns 0, not `-1`/`ENOMEM`.** If
  your fetch loop breaks on the first `rc == 0`, you get a correctly-*sized*
  but **zero-filled** buffer and silently attribute nothing. You must keep
  iterating after the probe: allocate, then fetch, and only stop once you've
  read into a non-null buffer (and still handle `ENOMEM` to grow if the table
  changed). This mirrors sockstat's `sysctl_sucker`; see `suckMib`.
* `_KMEMUSER` is a red herring here: `kinfo_pcb`/`kinfo_file` sizes are the
  same with or without it on NetBSD 11. sockstat defines it only for the
  `DTYPE_*` enum and some socketvar fields.
* The resolver sysctls work **non-root**; only the pcap capture needs root.
  Refresh once per snapshot tick — it's cheap and there is no per-flow cost.
* These struct layouts are **NetBSD-specific** (FreeBSD's `xinpcb` /
  `xsocket`, OpenBSD's variants differ). Keep the resolver `#if
  defined(__NetBSD__)`-guarded with a stub for the other BSDs so the backend
  still builds (capture works, flows are just unattributed) until someone adds
  the per-OS layout.

### 7.8 DBus is optional on BSD

On BSD the in-process backend is the natural, first-class path; do **not**
require a running DBus bus. The frontends probe for the agent and fall back
cleanly to in-process when no bus/agent is present (verified: no DBus warnings
on a NetBSD box with no session/system bus). A future BSD privileged agent
should consider a non-DBus IPC (or make DBus strictly optional).

### 7.9 What's portable vs OS-specific (for the next BSD)

* Portable as-is across all BSDs: the `getifaddrs` interface backend, the
  libpcap capture + parsing + flow accounting + SYN-direction, the curses and
  `getpeereid` shims, the CMake plumbing.
* Per-OS work for FreeBSD/OpenBSD/DragonFly: the process-attribution sysctl
  layer (different `kinfo_*` structs / MIBs) and, eventually, jail/container
  attribution (`jail_get(2)` on FreeBSD) and a privileged agent.
