# Portability — multi-OS / multi-datapath roadmap

> Status: **exploratory**. qiftop v0.1 is Linux-only by design. This
> document captures the current portability boundary, what would be
> needed to run on other operating systems or with alternative data
> sources, and which structural decisions to make early so a future
> port doesn't require a rewrite.

---

## 1. Current portability boundary

| Layer                | Today                            | Portability                                  |
|----------------------|----------------------------------|----------------------------------------------|
| `ui/`                | Qt 6 Widgets                     | ✅ Already portable to every Qt 6 target.    |
| `dns/`               | `QHostInfo`-based async resolver | ✅ Portable.                                 |
| `config/`            | `QSettings`                      | ✅ Portable.                                 |
| `util/Units`         | Pure C++/Qt                      | ✅ Portable.                                 |
| `util/ConnectionFilter` | Pure C++/Qt                   | ✅ Portable.                                 |
| `util/Exporter`      | Pure C++/Qt                      | ✅ Portable.                                 |
| `backend/` interfaces | Abstract `QObject` bases        | ✅ Portable.                                 |
| `backend/PlatformInfo` | POSIX `getifaddrs` + Linux sysctl with fallbacks | 🟡 Compiles everywhere, full data only on Linux. |
| `dbus/Types`         | Qt DBus marshalling              | 🟡 Compiles everywhere; **runs** wherever a system/session bus exists. |
| `backend/linux/`     | libnl-3 + libnetfilter_conntrack | 🔴 Linux-only.                               |
| `agent/`             | DBus daemon                      | 🟡 Logic portable; DBus availability varies. |
| `util/HandoffServer` | Unix socket + `SO_PEERCRED`      | 🟡 Linux/BSD only; macOS needs `LOCAL_PEERCRED`; Windows needs a different IPC. |
| `util/PrivilegeEscalator` | pkexec / sudo / kdesu      | 🔴 Linux-desktop-only.                       |
| `dist/systemd/`, `dist/dbus/`, `dist/debian/` | systemd + Debian | 🔴 Linux-distro-specific packaging.          |

Legend: ✅ portable / 🟡 partly / 🔴 OS-specific.

The **structural** picture is good: the abstract `NetworkMonitor`,
`ConnectionMonitor`, and now `ProcessResolver` interfaces are all
free of platform headers, and the platform-specific code lives in
`backend/<os>/` subdirectories. The two **biggest architectural**
roadblocks for non-Linux ports are DBus and the privilege model
(see §4).

---

## 2. Target platforms — data-source survey

### 2.1 Linux (current)

| Need                         | API                                                       |
|------------------------------|-----------------------------------------------------------|
| Per-interface counters       | rtnetlink via libnl-3 (`rtnl_link_*`)                     |
| Per-flow counters            | libnetfilter_conntrack (`NFCT_Q_DUMP`)                    |
| Local addresses              | `getifaddrs()`                                            |
| Ephemeral port range         | `/proc/sys/net/ipv4/ip_local_port_range`                  |
| Socket → PID                 | `NETLINK_SOCK_DIAG` + `/proc/<pid>/fd` walk (v0.2 step 2) |
| PID → cgroup/container       | `/proc/<pid>/cgroup` (v0.2 step 3)                        |
| Cross-netns flows            | `setns()` + sock_diag inside (v0.2 step 4)                |
| Privilege model              | `CAP_NET_ADMIN`, system DBus, pkexec                      |

### 2.2 FreeBSD / OpenBSD / NetBSD

| Need                         | API                                                       |
|------------------------------|-----------------------------------------------------------|
| Per-interface counters       | `sysctl net.link.generic.ifdata.*` or `getifaddrs()` AF_LINK |
| Per-flow counters            | `pfctl -ss` parse, or **libpfctl** (FreeBSD 13+), or BPF + per-flow accounting |
| Local addresses              | `getifaddrs()` ✅                                          |
| Ephemeral port range         | `sysctl net.inet.ip.portrange.first/last`                 |
| Socket → PID                 | `sysctl net.inet.tcp.pcblist` then `kvm_*` / `procstat`   |
| Container attribution        | jails: `jail_get(2)`; podman/containerd on FreeBSD = niche |
| Privilege model              | Setuid helper or capsicum-sandboxed daemon; no DBus by default |

Verdict: **moderate** lift; clean `backend/freebsd/` plus a no-DBus
IPC option (see §4.1) would land it.

### 2.3 macOS / Darwin

| Need                         | API                                                       |
|------------------------------|-----------------------------------------------------------|
| Per-interface counters       | `sysctl net.link.iflist` or `if_data`; or `PF_NDRV`       |
| Per-flow counters            | **No** kernel-level per-flow accounting comparable to conntrack. Options: `nettop`-style via private `NetworkStatistics` framework; `NEFilterDataProvider` (Network Extension, requires entitlement + System Extension); BPF (`/dev/bpf*`) traffic-mirroring + userspace flow tracker. |
| Local addresses              | `getifaddrs()` ✅                                          |
| Ephemeral port range         | `sysctl net.inet.ip.portrange.first/last`                 |
| Socket → PID                 | `proc_listpids` + `proc_pidfdinfo` (libproc); no privileges required for own uid |
| Container attribution        | N/A by default; could attribute Docker Desktop's `vpnkit` indirectly |
| Privilege model              | `SMJobBless` for a privileged helper, signed bundle, no DBus |

Verdict: **hard**. The big roadblock is per-flow accounting — there
is no equivalent of conntrack. A v1 macOS port would likely ship
only the per-interface view (libnl equivalent) and disable the
Connections tab until a Network Extension–based collector lands.
Network Extension System Extensions require a paid Apple Developer
ID + entitlement.

### 2.4 Windows

| Need                         | API                                                       |
|------------------------------|-----------------------------------------------------------|
| Per-interface counters       | `GetIfTable2` / `GetIfEntry2` (iphlpapi)                  |
| Per-flow counters            | `GetExtendedTcpTable` / `GetExtendedUdpTable` (snapshot only); ETW Microsoft-Windows-Kernel-Network provider for byte/packet deltas; or WinDivert (GPL) for true per-flow byte accounting |
| Local addresses              | `GetAdaptersAddresses`                                    |
| Ephemeral port range         | `GetTcpRange` / registry `MaxUserPort` (legacy)           |
| Socket → PID                 | `GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL)` — built-in, no privileges |
| Container attribution        | Hyper-V containers: ETW container-id field; Docker Desktop: WSL2 (delegate to Linux backend running inside WSL) |
| Privilege model              | UAC manifest, Windows Service; no DBus                    |

Verdict: **moderate** for the per-interface and per-PID flow view
(`GetExtendedTcpTable` is unique among the four OSes for being both
free and not requiring privileges). Real-time per-flow accounting
needs ETW or WinDivert, both of which add a deployment story.

---

## 3. Architectural decisions to preserve

These rules already hold today; they exist BECAUSE of future ports.
Reviewers/agents touching the codebase should keep them intact.

1. **`backend/` interfaces never include platform headers.** The
   `NetworkMonitor`, `ConnectionMonitor`, `ProcessResolver`, and
   (now) `PlatformInfo` headers are POSIX-/platform-clean. Concrete
   impls go in `backend/<os>/` (compiled subdir picked in
   `CMakeLists.txt`) or behind `qiftop::platform` (inline
   `#if defined(Q_OS_*)`).
2. **DTOs (`dbus/Types.h`) use kernel-neutral encodings on the wire.**
   IANA proto numbers, RFC 2863 oper-state, kernel ifindex, conntrack
   TCP states — every one is an external standard or has a documented
   fallback (TcpState::None for non-Linux conntrack-less ports). A
   future macOS port encodes its data into these same DTOs.
3. **Capability tokens, not Version compare.** Every new optional
   feature gets a token (`process-attribution`, `container-attribution`,
   `oper-state`, `tcp-state`…) which the resolver / service advertises
   only when the runtime probe succeeded. A macOS agent that can't
   provide `tcp-state` simply omits the token; the existing UI
   already hides the column when the token is absent.
4. **Compile-time options gate compile-in; runtime probes gate use.**
   See `QIFTOP_ENABLE_PROCESS_ATTRIBUTION` etc. — the same pattern
   should be used for any future macOS/Windows feature that needs
   an entitlement or driver.
5. **The factory pattern** (`createProcessResolver()`,
   `createNetworkMonitor()` in `main.cpp`'s `#ifdef` block) keeps
   `main.cpp` to ~5 lines per platform. New ports add a clause,
   never edit the call sites.

---

## 4. The four real roadblocks

### 4.1 DBus

The agent IPC is hard-coded to DBus today. macOS has no system bus;
Windows has none either; even on FreeBSD it's rarely installed.

**Plan B** that we should keep open: the `NetworkMonitor` /
`ConnectionMonitor` / `ProcessResolver` interfaces are pure
`QObject` signal emitters — they don't know they're behind DBus
proxies. We could add a `backend/local/LocalSocketTransport.{h,cpp}`
(or even Qt's `QLocalSocket` + a tiny CBOR framing) that serialises
the same DTOs. Same agent process, different transport. No DBus
required.

This isn't a v0.2 task, but **before adding any new DBus-only
method, ask whether the contract should be transport-agnostic**.
The `qiftop_dbus` static library is named for the transport; a
future `qiftop_ipc` could front both transports.

### 4.2 Privilege escalation

`pkexec` / Polkit is Linux-only. macOS has `SMJobBless` (now
deprecated in favour of `SMAppService`), Windows has UAC, FreeBSD
has setuid + capsicum. `util/PrivilegeEscalator` is already isolated
behind a small interface (`requestElevated()`); a future port adds
a sibling class without touching `MainWindow`.

### 4.3 Per-flow accounting on macOS

There is no kernel-level conntrack on macOS. Any port that ships
the Connections view there will need to either:
* Ship a System Extension (Network Extension API) — requires a paid
  Apple Developer ID, code signing, notarization, and a separately
  installed `.systemextension` bundle.
* Use BPF (`/dev/bpf*`) + userspace flow tracker — works without
  entitlements but requires CAP_NET_ADMIN-equivalent (root, or the
  user being in `_bpf` group).
* Disable the tab via capability token absence.

The third is the v1 escape hatch and the architecture supports it
today.

### 4.4 Packaging

`dist/debian/`, `dist/systemd/`, `dist/dbus/` are all Debian-flavour
Linux. macOS needs a `.pkg` or `.app` bundle, Windows needs an MSI
or NSIS installer, FreeBSD needs a port skeleton. None of this
affects the source layout; the install rules in `CMakeLists.txt`
just gain `if(APPLE)` / `if(WIN32)` clauses analogous to the
existing `if(UNIX AND NOT APPLE)` Linux block.

---

## 5. Concrete recommendations going forward

* **Keep `backend/PlatformInfo.{h,cpp}` as THE single place** for any
  "ask the kernel about the local host" call. New probes (e.g. PID
  for the currently-active user, available cgroup subsystems) live
  here behind `#if defined(Q_OS_*)`. The header stays platform-clean.
* **Never include `<linux/*>`, `<sys/un.h>`, `<netinet/*>`, or
  `<windows.h>` outside `backend/<os>/`** or inside `qiftop::platform`
  with documented fallbacks.
* **Every new DBus method ships with a token.** If you're tempted to
  add `agent.Version >= "0.4"` gating in the client, stop — the macOS
  agent will never expose `Version` in the same way. Use a token.
* **When in doubt, follow the `ProcessResolver` pattern.** Abstract
  interface in `backend/`, `null/` fallback that compiles everywhere,
  `<os>/` concrete impl, factory hides the `#ifdef`, capability
  tokens advertised at runtime. This is the v0.2 template for every
  future feature with platform-specific data sources.

---

## 6. What's NOT planned

To set expectations:

* macOS / Windows / *BSD ports are **not** v0.2 work. v0.2 is
  per-flow process+container attribution on Linux.
* No System Extension / notarized macOS app planned.
* No Hyper-V / WSL bridge planned.
* The DBus contract stays the contract for the foreseeable future;
  the `libqiftop` extraction (per AGENTS.md §2) is the path toward
  alternative transports if/when needed.
