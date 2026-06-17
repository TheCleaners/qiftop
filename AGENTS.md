# AGENTS.md — qiftop developer & contributor guide

A living document for humans and LLM-driven agents working on qiftop. Keep
each change in lock-step with the code so it stays usable. If you change a
public type, a build invariant, or a layering rule, update the relevant
section in the same commit.

---

## 1. What is qiftop?

`qiftop` is a Qt 6 iftop-style Linux network monitor. This tree builds
several component packages:

| Package(s)                         | Binary / content          | Privilege               | Talks to                                     |
|------------------------------------|---------------------------|-------------------------|----------------------------------------------|
| `qiftop`                           | Qt Widgets GUI            | Unprivileged GUI client | `qiftop-agent` over DBus (system or session) |
| `nqiftop`                          | ncurses TUI               | Unprivileged TUI client | `qiftop-agent` over DBus (system or session) |
| `qiftop-agent`                     | `qiftop-agent` daemon     | `CAP_NET_ADMIN` daemon  | Linux kernel (libnl-3, libnetfilter_conntrack) |
| `libqiftop0` / `qiftop-libs`       | `libqiftop.so.0` runtime  | none                    | DBus client + data/format helpers            |
| `libqiftop-dev` / `qiftop-devel`   | headers, CMake, pkg-config | none                   | build-time API for libqiftop consumers       |

In the normal packaged path, the agent is the component that touches the
kernel; GUI/TUI and example consumers stream from it over DBus. The GUI
retains the self-elevation fallback, and the TUI can use in-process capture
when run privileged, for machines where the agent isn't installed. DNS
resolution and any other "soft" enrichment happens strictly client-side.

---

## 2. Source layout

```
src/
├── agent/                    # privileged DBus daemon (qiftop-agent)
│   ├── main.cpp              # thin shim: argv, bus choice, constructs Application
│   ├── Application.{h,cpp}   # RAII bus surface: registers objects, name, IdleManager
│   ├── Config.{h,cpp}        # loadIdleConfig() — parses /etc/qiftop/agent.conf
│   ├── InterfacesService.{h,cpp}   # /.../Interfaces — Version + Capabilities props
│   ├── ConnectionsService.{h,cpp}  # /.../Connections — GetProcessDetails RPC, snapshot cap
│   ├── Attribution.{h,cpp}  # pure helper: enrich Connection list via ProcessResolver
│   └── IdleManager.{h,cpp}   # adaptive polling cadence + per-client hints
├── aggregate/                # libqiftop row/rate aggregators (plain QObject)
│   ├── InterfaceAggregator.{h,cpp}   # per-interface rates + sorted rows
│   └── ConnectionAggregator.{h,cpp}  # flow rates, EMA/tween, DNS, UDP peer aggregation
├── backend/                  # backend interfaces + platform impls
│   ├── NetworkMonitor.{h,cpp}      # abstract: per-interface stats
│   ├── ConnectionMonitor.{h,cpp}   # abstract: per-flow stats
│   ├── Connection.h                # Endpoint/Connection/L4Proto/Direction value types
│   ├── ProcessResolver.h           # abstract: pid/process/container/chain resolution
│   ├── ProcessDetails.h            # on-demand exe/cmdline/cwd/startTime DTO
│   ├── CompositeResolver.h         # fan-out across a chain of resolvers
│   ├── ProcessResolverFactory.{h,cpp}  # env-gated default resolver chain
│   ├── PlatformInfo.{h,cpp}        # qiftop::platform host facts (uid→name, ephemeral range)
│   ├── linux/                      # libnl + nf_conntrack + attribution (server-side)
│   │   ├── NetlinkMonitor.{h,cpp}, NetlinkWorker.{h,cpp}   # per-interface stats
│   │   ├── ConntrackMonitor.{h,cpp}                        # per-flow capture
│   │   ├── SockDiagResolver.{h,cpp}, SockDiagDump.{h}, SockDiagParse.h  # pid via sock_diag
│   │   ├── CgroupClassifier.{h,cpp}, CgroupParse.h         # pid → container runtime/id/chain
│   │   ├── NetnsScanner.{h,cpp}                            # per-netns socket dump (setns)
│   │   ├── ProcDetails.{h,cpp}                             # on-demand /proc/<pid> reads
│   │   └── ProcSnapshot.h                                  # /proc/<pid>/stat starttime parser
│   ├── dbus/                       # libqiftop DBus source proxies for consumers
│   │   ├── DBusNetworkMonitor.{h,cpp}
│   │   └── DBusConnectionMonitor.{h,cpp}
│   └── null/NullProcessResolver.h  # no-op resolver fallback
├── config/Settings.{h,cpp}   # QSettings-backed app prefs (Qt6::Core-only), emits changed()
├── dbus/Types.{h,cpp}        # DTOs + Qt marshalling for the wire format
├── dns/                      # client-side async DNS (never in the agent)
│   ├── DnsResolver.{h,cpp}         # abstract async resolver
│   └── QtDnsResolver.{h,cpp}       # QHostInfo-backed, LRU-cached
├── tui/                      # ncurses frontend (nqiftop), built on libqiftop
│   ├── main.cpp, TuiApp.{h,cpp}
│   └── Screen.{h,cpp}, TuiFormat.h, TuiTheme.h, Expansion.h
├── ui/                       # MainWindow, models, delegates, tray (Qt Widgets)
├── util/                     # Logging, Units, Exporter, ConnectionFilter, Autostart,
│                             #   PrivilegeEscalator, HandoffServer/Client (legacy elevation)
└── main.cpp                  # GUI entry point

examples/
├── ndjson-stream/            # interface snapshots as NDJSON
├── ndjson-connections/       # flow snapshots as NDJSON
├── prometheus-exporter/      # /metrics endpoint over libqiftop aggregators
├── snapshot-export/          # one-shot CSV/JSON export
└── top-talkers/              # headless iftop -t-style top-N printer

dist/
├── conf/agent.conf           # ships to /etc/qiftop/agent.conf (Debian conffile)
├── dbus/                     # bus policy + system-service activation file
├── debian/                   # postinst, postrm, conffiles
├── desktop/io.github.thecleaners.qiftop.{desktop,metainfo.xml,svg} + nqiftop.desktop + raster icons
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
5. **`aggregate/`, `util/`, `dbus/`, `config/`, `dns/`, and `backend/`
   (including `backend/dbus/` client proxies, excluding `backend/linux/`
   from the installed headers) must not include from `ui/` or depend on
   Qt Widgets.** These directories form the Widgets-free `libqiftop`
   public surface (Qt Core/Network/DBus). Anything pulling in `QWidget`,
   `QAbstract*Model`, `QSortFilterProxyModel`, or similar belongs in
   `ui/`. Keep pure helpers such as `util/ConnectionHeuristics.h` free of
   Network/DBus includes too so a future `libqiftop-core` split stays
   mechanical.
6. **No platform headers outside `backend/<os>/` or `backend/PlatformInfo.cpp`.**
   `<linux/*>`, `<sys/un.h>`, `<netinet/*>`, `<ifaddrs.h>`, `<windows.h>`,
   `<sys/sysctl.h>` etc. live in the platform subdirectory's translation
   units, or behind `qiftop::platform` (with documented fallbacks on
   unsupported OSes). Code in `agent/`, `ui/`, `dns/`, `config/`,
   `dbus/` and the abstract `backend/*.h` interfaces must compile on
   any Qt 6 target. See `docs/PORTABILITY.md` for the full survey of
   what each target OS would need.

### libqiftop and future direction

The third component now exists: **`libqiftop`** is the Widgets-free shared
data facility (`OUTPUT_NAME qiftop`, SONAME `libqiftop.so.0`) used by the
GUI, the `nqiftop` TUI, the agent, and standalone consumers. Its public
surface is the DBus wire DTOs, DBus client proxies, abstract monitors,
`InterfaceAggregator` / `ConnectionAggregator`, `Settings`, DNS helpers,
the filter mini-language, IEC unit formatters, and JSON/CSV exporter. It
installs CMake and pkg-config metadata (`find_package(qiftop)` →
`qiftop::qiftop`) and is packaged as `libqiftop0` / `libqiftop-dev`
on Debian and `qiftop-libs` / `qiftop-devel` on RPM distros. See
[`docs/LIBQIFTOP.md`](docs/LIBQIFTOP.md) for the authoritative consumer
guide.

Shipped example consumers live under `examples/`: `ndjson-stream`,
`ndjson-connections`, `prometheus-exporter`, `snapshot-export`, and
`top-talkers`. They are standalone `find_package(qiftop)` projects and
should remain thin source → aggregator → serialise examples, not forks of
GUI logic.

The v0.3 alerting direction is designed but not built as a new full stack:
prefer integration-first alerting (the Prometheus/OpenMetrics exporter +
Prometheus/Alertmanager; a planned Nagios check; and, only where useful, a
thin `qiftop-alertd` that reuses the existing `ConnectionFilter` grammar).
The implications stay the same:

* The DBus contract is effectively a public ABI for all consumers — treat
  DTO breakage as a multi-frontend cost, not just a GUI cost.
* Resist any change that tightens Widgets coupling in libqiftop-facing code.
  If you catch yourself reaching for `QAbstractItemModel` in a utility,
  that's a smell — the model should wrap a plain `QObject` aggregator, not
  be the aggregator.

---

## 3. Build, run, package

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run the agent against the session bus (no root needed, conntrack will warn)
./build/qiftop-agent --session --verbose -c dist/conf/agent.conf

# Run the GUI
./build/qiftop

# Build component .debs
cd build && cpack -G DEB
```

CPack quirk worth remembering: component-specific keys use the **upper-cased
component name verbatim, hyphens kept literal** —
`CPACK_DEBIAN_QIFTOP-AGENT_PACKAGE_*`.

---

## 4. The DBus contract

Well-known name: `org.qiftop.NetworkAgent1` (system bus in production;
`--session` for development).

| Object path                                  | Interface                                  | Methods                                                   | Signals                                                | Properties                  |
|----------------------------------------------|--------------------------------------------|-----------------------------------------------------------|--------------------------------------------------------|-----------------------------|
| `/org/qiftop/NetworkAgent1/Interfaces`       | `org.qiftop.NetworkAgent1.Interfaces`      | `GetInterfaces()`, `SetDesiredIntervalMs(u)`              | `StatsChanged(t, a(...))`, `CadenceChanged(u)`         | `Version: s`, `Capabilities: as` |
| `/org/qiftop/NetworkAgent1/Connections`      | `org.qiftop.NetworkAgent1.Connections`     | `GetConnections()`, `GetProcessDetails(u) → (uusssst)`, `SetDesiredIntervalMs(u)` | `ConnectionsChanged(t, a(...))`, `PermissionDenied`, `AccountingChanged` | |

Both data signals carry a leading `quint64 monotonicMs` (a
`QElapsedTimer`-based, agent-process-local monotonic millisecond
counter) sampled at the moment the snapshot was serialised. This is
the canonical timestamp for downstream rate computation (libqiftop
history, Prometheus exporter, alerts): it's immune to wall-clock
jumps and to DBus delivery jitter. It is NOT comparable across agent
restarts.

DTOs live in `src/dbus/Types.h`.

**Connection wire signature** (23 outer fields + nested chain):
`a(yysqysqttttsyuyuussssa(sss)y)` =
`(proto, localFamily, localAddress, localPort, remoteFamily, remoteAddress,
remotePort, rxBytes, txBytes, rxPackets, txPackets, iface, direction,
ifIndex, tcpState, pid, uid, comm, containerRuntime, containerId,
containerName, containerChain[a(sss)], reason)`.

Each `containerChain` entry is `(runtime, id, name)` — outer-to-inner
nesting, leaf entry equals `(containerRuntime, containerId, containerName)`
when both are populated.

**InterfaceStats wire signature** (16 fields per row):
`(ssuasttttbbuytttt)` =
`(name, type, mtu, addresses, rxBytes, txBytes, rxPackets, txPackets,
isUp, isLoopback, ifIndex, operState, rxErrors, txErrors, rxDropped,
txDropped)`.

Per-field notes:

* `proto` — **IANA L4 number** (RFC 5237): TCP=6, UDP=17, ICMP=1,
  ICMPv6=58, Unknown=0. See `toIanaProto` / `fromIanaProto` in
  `src/backend/Connection.h`. Capability: `iana-proto`.
* `direction` — `0=Unknown / 1=Outbound / 2=Inbound`. Computed
  server-side via `qiftop::heuristics::inferDirection`. Clients SHOULD
  trust non-zero values and fall back to a local heuristic only when
  the wire says `Unknown` (true today for in-process / handoff
  backends). `fromDto` clamps out-of-range values to `Unknown` rather
  than `static_cast`-ing them (UB-safe against buggy/future agents).
  Capability: `direction-on-wire`.
* `ifIndex` — Kernel ifindex matching `iface` / `name`. Prefer this
  over the iface name string for stable identity (names can be reused
  across netns; ifindex cannot within a single namespace). 0 = unknown
  or unattributed. Capability: `ifindex`.
* `operState` — Linux `IF_OPER_*` (RFC 2863): 0 UNKNOWN, 1 NOTPRESENT,
  2 DOWN, 3 LOWERLAYERDOWN, 4 TESTING, 5 DORMANT, 6 UP. Distinguishes
  "admin up but link not up" from "admin down" — the simple `isUp`
  bool only tells you about IFF_UP (admin). Capability: `oper-state`.
* `tcpState` — Conntrack TCP state (`TCP_CONNTRACK_*` per
  `<linux/netfilter/nf_conntrack_tcp.h>`): 0 NONE, 1 SYN_SENT, 2
  SYN_RECV, 3 ESTABLISHED, 4 FIN_WAIT, 5 CLOSE_WAIT, 6 LAST_ACK, 7
  TIME_WAIT, 8 CLOSE, 9 SYN_SENT2. Non-TCP flows always report 0.
  `fromDto` clamps unknown values to NONE. Capability: `tcp-state`.
* `rxErrors` / `txErrors` / `rxDropped` / `txDropped` — Cumulative
  kernel counters via libnl `RTNL_LINK_RX/TX_ERRORS / RX/TX_DROPPED`.
  Useful for surfacing flaky NICs and tight-budget tunnels. Capability:
  `link-errors`.
* `pid` / `uid` / `comm` — Best-effort process attribution for a flow.
  `pid == 0` means unattributed (no resolver wired, or the flow's
  socket couldn't be located via SOCK_DIAG within the cache window —
  or the flow is genuinely forwarded, e.g. a VM-bridge / k8s-pod-netns
  flow with no host socket). `SockDiagResolver` indexes each socket by
  BOTH its full 4-tuple AND a local-only 2-tuple (proto + local
  addr/port); `resolvePid` tries 4-tuple → exact local 2-tuple →
  wildcard (`0.0.0.0`/`::`) local 2-tuple. The local fallback is what
  attributes unconnected UDP sockets and TCP listeners, whose
  `idiag_dst` is `0.0.0.0:0` and so can never match a live flow's real
  remote by the 4-tuple alone. `comm` is the kernel-truncated 15-byte
  basename. Expensive fields
  (`exe`, `cmdline`, `cwd`) are deliberately NOT shipped on the wire —
  fetched on demand via `GetProcessDetails(pid)` per the "default-cheap
  pipeline" design principle. Capability: `process-attribution-wire`.
* `containerRuntime` / `containerId` / `containerName` — Best-effort
  container-scope attribution. `containerRuntime` is lowercase
  (`"docker"`, `"containerd"`, `"podman"`, `"kubernetes"`, `"systemd"`,
  `"lxc"`, `"lxd"`, `"nspawn"`). `containerId` is the 12-char hex
  prefix for content-addressable runtimes and the full human name for
  name-ID runtimes (lxd/lxc/nspawn) — see `docs/ATTRIBUTION.md §5c`.
  All three empty when the flow has no container scope. Capability:
  `container-attribution-wire`.
* `containerChain` — Nested ancestry, outer-to-inner. Empty when no
  nesting was detected. When non-empty the leaf entry equals
  `(containerRuntime, containerId, containerName)`. Examples: a flow
  inside a k8s-managed containerd pod yields
  `[{"kubernetes", "<pod-uid>", ""}, {"containerd", "<cid>", ""}]`
  (depth 2); a flow inside docker-in-docker can reach depth 3.
  Capability: `container-chain-wire`.
* `reason` — Why the flow is / isn't attributed to a local process
  (`AttributionReason`): `0=Resolved` (pid > 0), `1=NoLocalSocket`
  (local endpoint is ours but no live socket was found — a closed UDP
  flow still in conntrack, a kernel socket, or a genuine miss),
  `2=Orphaned` (TCP teardown state — the owning socket is gone), `3=
  Forwarded` (routed/NAT/masquerade: neither endpoint is a host
  address, so there is no local process by design). Computed
  server-side after attribution via `heuristics::attributionReason`
  (reuses the same host-address context as direction inference).
  `fromDto` clamps unknown values to `NoLocalSocket`. Clients without
  the token derive it locally via the same shared heuristic.
  Capability: `attribution-reason`.

### Contract version & capabilities

`Version` is a free-form string (currently `"0.6"`) bumped only for
*additive* changes to the agent surface that clients may care about.
**Breaking** changes (DTO signature, method removal/rename) still require
a fresh `org.qiftop.NetworkAgent2` interface per §8.

> Historical note: the 0.1 → 0.2 bump (pre-release v0.1-alpha2 →
> alpha3) reshaped the wire (added `direction` byte, switched `proto`
> to IANA, PascalCased `AccountingChanged`). The 0.2 → 0.3 bump
> (alpha3 → v0.1) extended both DTOs and added a `monotonicMs` leading
> arg to both data signals. The 0.3 → 0.4 bump (v0.2 attribution work)
> appended 7 fields to `ConnectionDto` (`pid, uid, comm,
> containerRuntime, containerId, containerName, containerChain`) for
> bulk process + container attribution. The 0.4 → 0.5 bump added the
> on-demand `Connections.GetProcessDetails(pid)` RPC for fetching
> `exe`/`cmdline`/`cwd`/`startTime` lazily — additive (no DTO
> change). The 0.5 → 0.6 bump appended a `reason` byte
> (`AttributionReason`) to `ConnectionDto` so consumers can tell a
> forwarded/orphaned flow (no local process by design) from a genuine
> attribution miss. Since only pre-release alphas existed in those
> windows we reshaped in place rather than branching `NetworkAgent2`.
> Older alpha clients failing to unmarshal fall back cleanly to the
> in-process backend via the existing probe. Post-v0.1 stable
> release, breaking changes MUST go through `NetworkAgent2`.

`Capabilities` is a `QStringList` of dash-separated lowercase tokens.
Clients gate optional behaviour on token presence — never on a Version
comparison — and treat absence as "off" so they keep working against
older agents. Tokens currently emitted:

| Token                 | Meaning                                                     |
|-----------------------|-------------------------------------------------------------|
| `cadence-hints`       | `SetDesiredIntervalMs` is honoured (vs. ignored).           |
| `cadence-signal`      | `CadenceChanged` fires on effective-cadence changes.        |
| `name-owner-cleanup`  | Hints dropped on `NameOwnerChanged` (peer disconnect).      |
| `monotonic-clock`     | Hint TTL uses `CLOCK_MONOTONIC` (immune to wall-clock jumps). |
| `snapshot-cap`        | `ConnectionsChanged` payload is capped at top-N by bytes.   |
| `iana-proto`          | `ConnectionDto.proto` is an IANA number (RFC 5237), not the internal enum index. |
| `direction-on-wire`   | `ConnectionDto.direction` is populated server-side; clients can skip the heuristic when non-zero. |
| `snapshot-timestamp`  | `StatsChanged` / `ConnectionsChanged` prefix the payload with a CLOCK_MONOTONIC ms counter. |
| `ifindex`             | `InterfaceStatsDto.ifIndex` and `ConnectionDto.ifIndex` populated. |
| `oper-state`          | `InterfaceStatsDto.operState` populated with `IF_OPER_*` per RFC 2863. |
| `link-errors`         | `InterfaceStatsDto` carries `rxErrors` / `txErrors` / `rxDropped` / `txDropped`. |
| `tcp-state`           | `ConnectionDto.tcpState` populated for TCP flows.           |
| `attribution-reason`  | `ConnectionDto.reason` populated server-side (`AttributionReason`: Resolved/NoLocalSocket/Orphaned/Forwarded) so clients can distinguish "no local process by design" (routed/NAT, torn-down socket) from a genuine attribution miss. Advertised unconditionally — the reason is derivable even with no resolver wired. |
| `process-attribution` | Resolver chain provides per-flow PID / comm / uid (Linux `SockDiagResolver`). UI-visible once `ConnectionDtoV2` lands. |
| `container-attribution` | Resolver chain provides per-PID container runtime + id + name (Linux `CgroupClassifier`). |
| `container-chain`     | Resolver chain exposes the full OUTER→INNER container nesting via `ProcessResolver::resolveContainerChainForPid` (Linux `CgroupClassifier`). Single-attribution consumers can ignore this and keep calling `resolveContainerForPid`. |
| `netns-scan`          | Resolver chain dumps sock_diag in every non-host network namespace (Linux `NetnsScanner`, requires `CAP_SYS_ADMIN`). |
| `process-attribution-wire` | `ConnectionDto` carries `pid`, `uid`, `comm` populated server-side. Implies the agent has a `process-attribution`-capable resolver wired. Mirror token so non-resolver-aware clients gate UI on this rather than poking attribution fields blindly. |
| `container-attribution-wire` | `ConnectionDto` carries `containerRuntime`, `containerId`, `containerName` populated server-side. Implies `container-attribution` resolver capability. |
| `container-chain-wire` | `ConnectionDto.containerChain` is populated when applicable. Strict superset of `container-attribution-wire`: a flow with no nesting still yields a single-entry chain. Advertised when the resolver provides BOTH `container-attribution` AND `container-chain` (see Application.cpp). Today CgroupClassifier always ships both, but the two flags must stay separable. |
| `on-demand-process-details` | `Connections.GetProcessDetails(pid u) → (pid u, uid u, comm s, exe s, cmdline s, cwd s, startTimeJiffies t)` RPC is available. Returns an all-zero struct (pid=0) on unknown / disappeared PID, never a DBus error. Cache key for clients is `(pid, startTimeJiffies)` — startTime distinguishes PID reuse within one boot. Advertised unconditionally on Linux; non-Linux backends return empty. |

Add a token here when shipping a new optional behaviour; **never remove
or rename a token** — pre-existing clients use them as feature flags.

### Wire-contract wisdom

Lessons accumulated across the v0.1 contract reviews (#1 and #2). When
designing or extending any DBus method/signal/DTO, prefer these defaults
over re-deriving them each time:

* **Don't ship internal enum *indices* across the wire.** Use an
  external, stable encoding (IANA numbers, IETF RFCs, kernel UAPI
  values) so non-Qt consumers don't need to mirror our private
  `enum class` declarations. The 0.1 wire shipped `L4Proto` indices
  (TCP=1); the 0.2 wire ships IANA numbers (TCP=6). The latter is
  recoverable by anyone with an RFC 5237 lookup; the former required
  reading our header.
* **Clamp every wire-sourced enum on the receive path.** Use a range
  check + fallback to a known-safe value instead of `static_cast<E>(x)`
  when `x` comes from a peer process. UB on the receiver from a
  malicious or future-extended sender is otherwise free. See
  `fromDto`'s direction/tcpState clamps.
* **Append-only struct fields.** DBus struct sigs hash the field list;
  reordering or removing breaks every subscriber. Always add new
  fields at the END of the DTO. The `<<`/`>>` operators must list
  fields in declaration order; mismatch = silent off-by-one on the
  wire.
* **Demarshallers MUST tolerate a SHORTER struct (new client ↔ old
  agent).** Append-only protects old-client/new-agent, but the reverse
  — a NEWER client whose reader expects the latest field reading an
  OLDER agent's shorter struct — will read PAST the end of the
  structure and `Aborted` the process under QtDBus (`type invalid 0
  not a basic type`). A real user hit this (new `nqiftop` vs a v0.5
  agent). Two rules, both in `src/dbus/Types.cpp`:
  (1) guard every trailing (post-base) field in `operator>>` with
  `if (!a.atEnd()) a >> field;` so missing tail fields keep their
  member defaults; (2) on the CLIENT, demarshal the RAW reply message
  (`watcher->reply()` → `args.at(0).value<QDBusArgument>() >> list`),
  NOT `QDBusPendingReply<DtoList>` — the typed reply is rejected
  wholesale on a signature mismatch (`unexpected reply signature`)
  before our tolerant `operator>>` ever runs (see
  `DBus{Connection,Network}Monitor`). The signal path already takes
  the raw `QDBusMessage`, so it only needs rule (1). Pinned by
  `test_wire_compat` (a real session-bus round-trip: an old-shaped
  service struct decoded by the production reader; aborts without the
  guards).
* **Capability tokens are append-only too.** Once a token is shipped,
  it must keep its name forever (clients use presence as a feature
  flag). Add new tokens for new behaviour; never delete or rename.
* **Tokens advertise BEHAVIOUR, not version.** `cadence-hints` means
  "the agent honours hints", not "agent ≥ 0.1". Clients should always
  branch on token presence, never on `Version` comparison.
* **Snapshot signals should carry their own timestamp.** Receipt time
  on the client is noisy (DBus delivery jitter, GC pauses, UI
  hangs). Sample the timestamp at the producer at serialise time.
  Use `CLOCK_MONOTONIC` (or `QElapsedTimer::elapsed()`) not wall
  clock — wall-clock jumps WILL corrupt rate calculations the moment
  NTP or systemd-timesyncd nudges the clock.
* **Cap snapshot payloads at the producer.** On a busy router
  conntrack can hold 100 k+ flows; a 10 MB DBus message every second
  hurts both sides and `m_last` will pin the high-water mark for the
  life of the process. Sort by importance, truncate, log a `qWarning`.
* **Server-side enrichment beats client-side enrichment for shared
  derivations.** Direction inference and ifindex lookup both used to
  be (or would have been) client-side; moving them server-side
  means each new frontend (libqiftop, exporter, TUI) doesn't have
  to reimplement them. The cost is one inferDirection call per
  capped flow per tick — trivial compared to the conntrack dump
  itself.
* **Stable identity ≠ display name.** Always ship the kernel index
  (ifindex) alongside the human name (ifname). Names get reused
  across netns; indices don't within one namespace.
* **Pre-release breaking changes are cheap; post-release ones are
  not.** While only `vX.Y-alphaN` tags exist publicly, reshape the
  wire freely and bump the additive `Version` field. After the first
  stable tag, breaking changes MUST branch a new interface name
  (`NetworkAgent2`) and keep `NetworkAgent1` alive for at least one
  release.
* **Bump `Version` AND add a capability token for every additive
  change.** Version tells the user which agent build they're talking
  to; the capability token is what clients actually branch on.
  Different jobs, both required.
* **The wire schema lives in two places** — `src/dbus/Types.{h,cpp}`
  (the C++ source of truth) and `AGENTS.md §4` (the human-readable
  contract). They drift trivially. Update both in the same commit, or
  the next contract review wastes time re-discovering the drift (we
  did, in review #2: the signature string was `(yysqysqtttts)` —
  only 12 type chars — while the prose claimed 13 named fields).

### Bounded payloads

`ConnectionsService` caps each emitted `ConnectionsChanged` snapshot at
**4096 flows**, sorted by `bytes_in + bytes_out` (top talkers). When the
kernel table is larger, the agent logs a `qWarning` and truncates; the
GUI never has to deal with a million-row table. The cap is a compile-time
constant in `src/agent/ConnectionsService.cpp` — bump it (and the §4
contract note) if a real use case appears.

The **in-process** `ConntrackMonitor` (GUI self-elevation / no-agent
fallback) applies the **same** cap (`kMaxInProcessFlows = 4096`, keep in
sync with the agent's `kMaxConnections`), but collects the top-N via a
K-bounded min-heap by bytes (`backend/linux/FlowTopK.h::admitFlowTopK`)
so the transient memory is O(K), not O(table). Same one-time `qWarning`
on overflow. Pinned by `test_flow_topk`.

### Access control

The system-bus policy (`dist/dbus/org.qiftop.NetworkAgent1.conf`) restricts
all method calls and signal reception to members of the `netdev` group.
This closes two issues that the previous "any local user" policy left
open:

* **CPU/netlink amplification DoS.** Any caller could pin the root agent
  at `poll.min_interval_ms` (100 ms) indefinitely by re-asserting a
  cadence hint every few seconds. Each hint also called `noteActivity()`,
  so the IdleManager never wound down.
* **Information disclosure.** `GetConnections()` returns the full
  conntrack table — every flow on the host, including other users'
  source ports and peer IPs. `/proc/net/nf_conntrack` is root-only on
  most distros; the agent must not demote that.
* **`GetProcessDetails` exposure tradeoff.** The on-demand RPC always
  returns the low-sensitivity bulk fields (`pid`/`uid`/`comm`/
  `startTimeJiffies`, the same already exposed per-flow by
  `GetConnections`) to any netdev-group caller. The privileged fields —
  `exe`/`cwd` symlink targets (normally mode-0700, readable only by the
  process owner and root, followed here only because the agent holds
  `CAP_SYS_PTRACE` / `CAP_DAC_READ_SEARCH`) and `cmdline` (world-readable
  but can leak credentials passed on the command line) — are disclosed
  **only when the D-Bus caller is root or owns the target PID**
  (`ConnectionsService::callerMaySeeProcessFields`, via
  `QDBusConnectionInterface::serviceUid`, fail-safe to redaction when the
  caller uid can't be established). A netdev user inspecting their own
  processes (and the GUI run as that user) sees everything; a netdev user
  probing *another* user's PID gets only pid/uid/comm. This keeps the
  attribution UX intact while not letting netdev membership alone
  escalate into cross-UID `exe`/`cwd`/`cmdline` disclosure. The gate is
  **configurable** via `[process_details] disclosure` in
  `/etc/qiftop/agent.conf` (§5): `owner` (default, the behaviour above),
  `permissive` (any netdev caller — the pre-0.2.1 behaviour), or
  `restricted` (root/owner plus an `allow_users` / `allow_groups`
  allowlist, e.g. `wheel`, for cross-UID admin visibility). Distros that
  want an even stricter gate can still narrow `GetProcessDetails` in the
  bus policy file.

`dist/debian/postinst` ensures the `netdev` group exists and, when
installed via `sudo apt install` / `pkexec`, automatically adds the
invoking user (`$SUDO_USER` / `$PKEXEC_UID`) to it. Users still need to
log out and back in (or run `newgrp netdev`) for the new group
membership to take effect. The GUI's
`probeAgent()` does a real `GetInterfaces` call with a 1 s timeout (and
opportunistically reads `Version` / `Capabilities`), so users without
group access fall back cleanly to the in-process (self-elevated) backend
rather than seeing an empty UI. The active mode is shown in the GUI
status bar (`agent <version>` vs. `in-process`).

When adding a new method, no XML edit is required — the group gate
applies at the interface level. If a specific method needs a stricter
gate (e.g. an admin-only setter), add a `<deny send_member="…"/>` to
the default policy and a matching `<allow>` to the privileged stanza.

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
* Setting any window or `idle.timeout_secs` to `0` disables that step
  (the comparison is guarded; see `IdleManager::evaluate`). This matches
  what `dist/conf/agent.conf` has always documented.
* **Client heartbeat is gated on UI visibility (PERF-L2).** The GUI's
  heartbeat lives in `MainWindow::refreshAgentHeartbeat()` and is
  asserted only while `isVisible()`. When the window is hidden to tray
  (or never shown under `--tray`), the heartbeat STOPS, so the agent's
  idle wind-down runs and it eventually pauses — even though the tray
  sparkline only needs cheap interface stats, the single shared
  `IdleManager` drives both services at one cadence, so there is no way
  to keep interfaces fast while letting connections idle. `showEvent()`
  re-asserts immediately, waking the agent the instant the window is
  summoned. Plain minimize keeps the window "visible" and does NOT
  suspend the heartbeat.

---

## 5. Configuration

`/etc/qiftop/agent.conf` — INI parsed by `QSettings(IniFormat)`, read once
at startup. See the shipped file (`dist/conf/agent.conf`) for the
authoritative documentation of every key. Marked as a Debian conffile so
user edits survive package upgrades.

Every parsed value is routed through a `clampCfg()` helper that bounds it
to a sensible range and emits a `qWarning()` if the file value is out of
range (intervals: `[10 ms, 1 h]`; windows/timeouts: `[0, 24 h]`, with `0`
meaning "disable that step"). Typos in the conffile produce a visible
warning, not a degenerate cadence.

The `[attribution]` section (loaded by `loadAttributionConfig`, separate
from `loadIdleConfig`) controls the resolver chain at **startup only** —
no D-Bus runtime override, Version bump, new token, or async deep pass is
part of this config-only knob set. `eagerness` is `balanced` by default:
host sock_diag/proc refresh at 1000 ms, cgroup cache at 2000 ms, and
cross-netns scanning at 5000 ms (the historical production cadence).
`eager` tightens those to 250 / 1000 / 1000 ms for machines where fresher
attribution is worth the extra `/proc` and `setns` churn. `off` is the
hard kill switch: the factory builds `NullProcessResolver`, so process /
container / netns capability tokens naturally disappear without inventing
new wire semantics. Per-layer booleans (`process`, `container`, `netns`)
can only disable compiled-in layers; `process=false` also disables the
dependent container/netns layers and logs one startup warning if they were
otherwise true. Advanced overrides `cache_refresh_ms` (`0` or
`[100, 60000]`) and `netns_refresh_ms` (`0` or `[250, 300000]`) apply
after the preset; invalid non-zero values warn and fall back to the
preset. The concrete resolver constructors keep the same floors as a last
ditch safety rail, because a 1 ms root `/proc` blender is not a feature.

The `[process_details]` section (loaded by `loadProcessDetailsPolicy`,
separate from `loadIdleConfig`) controls who may see the privileged
`exe`/`cwd`/`cmdline` fields from `GetProcessDetails` — see §4. Keys:
`disclosure` (`owner` default / `permissive` / `restricted`), and the
`restricted`-mode `allow_users` / `allow_groups` allowlists (comma- or
whitespace-separated; groups match primary or supplementary membership
via `qiftop::platform::userInGroup`). An unrecognised `disclosure` value
warns and falls back to `owner`.

Override path: `qiftop-agent --config <path>`.

---

## 6. Testing strategy

Tests live in `tests/`, gated by `option(QIFTOP_BUILD_TESTS ON)` (on by
default; turn off for distro builds that don't want QtTest in the build
deps). Each test is its own executable so a single test's crash doesn't
take the rest down. Run with `ctest --test-dir build --output-on-failure`.

### 6.1 Tiers

| Tier              | What                                                  | Privilege     | Where                                |
|-------------------|-------------------------------------------------------|---------------|--------------------------------------|
| **unit**          | Pure logic, no I/O, no DBus, no kernel.               | none          | `tests/`                             |
| **integration**   | Spin up `qiftop-agent --session` + drive over DBus.   | none          | `tests/test_agent_integration.cpp`   |
| **end-to-end**    | Real system bus, real conntrack. Manual / CI runner.  | root          | (not yet)                            |
| **benchmark**     | `QBENCHMARK` microbenchmarks of hot paths (aggregator, filter, top-K, …). Opt-in (`QIFTOP_BUILD_BENCHMARKS=ON`), `EXCLUDE_FROM_ALL` + `DISABLED` so the default `ctest` never runs them. No new deps. | none | `bench/` |

Benchmarks (`bench/`) compile the specific `src/` files they need directly
(like the tests, not via `libqiftop`) and draw inputs from deterministic
seeded generators in `bench/BenchData.h`. See `docs/HACKING.md` §5.8 for the
run recipe and the current baseline numbers (used to size the resolver
eagerness/cadence budget). Resolver/conntrack benches that need a live kernel
are an integration-tier follow-up, not part of the pure `bench/` set.

### 6.2 What's currently covered

| Test                       | Subject                                                           |
|----------------------------|-------------------------------------------------------------------|
| `test_direction`           | `inferDirection` (ephemeral-port + local-end fallback)            |
| `test_forwarded`           | `isForwardedFlow` + `attributionReason` heuristics (Resolved/Forwarded/Orphaned/NoLocalSocket; PID always wins) |
| `test_ema`                 | `emaUpdate`, `easeOutCubic`                                       |
| `test_interface_aggregator` | `InterfaceAggregator` row identity, sorted snapshots, and per-interface rate computation without Qt model/view. |
| `test_connection_aggregator` | `ConnectionAggregator` flow insertion/update/removal, raw rates, stale pruning, UDP peer aggregation, and copy helpers. |
| `test_tui_format`          | Pure TUI formatting/sorting/grouping/detail helpers (`TuiFormat.h`, `Expansion.h`) over aggregator rows — no ncurses event loop. Includes `orderedGroupIndices` (the group-display-order policy behind nqiftop's `sortWithinGroups`: frozen first-appearance vs classic aggregate order + stable tiebreak), `wrapToWidth` (word-wrap + hard-break for the modal dialogs' wrapped value column), `groupDetailRows` (the Enter-on-header group-info window: bulk fields + aggregates, plus exe/cmdline/cwd once on-demand details arrive), and the grouped-redundancy parity guard (`cellsForConnection` flow cell never repeats the comm/container the group header carries — the TUI analogue of the GUI hiding the redundant attribution column when grouped by that key). |
| `test_tui_theme`           | Built-in `nqiftop` themes, case-insensitive lookup, fallback, and direction colour/attribute separation. |
| `test_settings_migration`  | `Settings` legacy-key migration logic; chip-colour + v0.2 attribution view settings (view mode, process/container column toggles, chain-in-tooltip) round-trip + out-of-range view-mode clamp |
| `test_autostart`           | XDG autostart file lifecycle (`util/Autostart`)                   |
| `test_exporter`            | JSON quint64-as-string, qint64 numeric, CSV formula-injection     |
| `test_idle`                | `IdleManager` cadence, hints, TTL, 64-cap, degrade, NameOwnerChanged |
| `test_dns_cache`           | `QtDnsResolver` LRU bound, batch eviction, key dedup              |
| `test_handoff_auth`        | `HandoffServer` nonce auth (HELLO + 256-bit), pre-auth size cap   |
| `test_proxies`             | `ConnectionFilterProxy` + `InterfaceFilterProxy` visibility rules |
| `test_agent_config`        | `qiftop::agent::loadIdleConfig` (defaults, schedule, clamp, tolerance) |
| `test_agent_integration`   | Spawns real `qiftop-agent --session`, drives Version/Capabilities/GetInterfaces/SetDesiredIntervalMs end-to-end |
| `test_units`               | `util::formatBytes` / `formatByteRate` IEC unit boundaries + precision |
| `test_priv_escalator`      | `PrivilegeEscalator::envAllowlist` / `filterEnv` — security-critical env-var filtering for the root child |
| `test_dbus_types`          | `ConnectionDto` wire round-trip: IANA proto mapping, direction field, out-of-range direction clamp; v0.4 attribution round-trip + defaults; v0.5 `reason` round-trip + out-of-range clamp to NoLocalSocket. |
| `test_wire_compat`         | Forward-compat: a real session-bus service returns OLD-shaped (shorter) `ConnectionDto`/`InterfaceStatsDto` structs (no `reason` / no v0.3 iface tail); the PRODUCTION `operator>>` must decode them via its `atEnd()` guards (missing tail fields default) instead of reading past the struct end and aborting — the new-client-vs-old-agent crash. |
| `test_attribution`         | `agent::attributeFlows` — null-resolver no-op, process-only / container-only / chain attribution paths, per-PID memoisation (50 flows from same PID = 1 container lookup), chain opt-in obeys `wantContainerChain` flag, flow without PID never triggers `resolveContainerForPid(0)`. Uses a FakeResolver — no /proc, no sock_diag. |
| `test_composite_resolver`  | `qiftop::backend::CompositeResolver` — empty composite is a no-op; first-non-nullopt fan-out for resolvePid / enrichPid / resolveContainerForPid; capability tokens are unioned and de-duplicated in first-seen order; `resolveContainerChainForPid` deliberately bypasses the base-class single-wrap fallback so chain-capable children get to provide the real OUTER→INNER ancestry; initialize() probes EVERY child (not short-circuited). Uses a programmable FakeResolver — no Qt Widgets, no DBus. |
| `test_proc_details`        | `readProcessDetails` (Linux on-demand RPC backend) — invalid/missing PID returns `valid=false` without crashing; self-PID round-trips pid/uid/cmdline/exe; `/proc/<pid>/stat` field-22 starttime parser is non-zero; alternate procRoot parameter is honoured (fixtureability seam). |
| `test_mainwindow_smoke`    | Offscreen widget smoke coverage for MainWindow construction, sorting/filtering/grouping, settings propagation, attribution capability gates, stale rows, DNS rerendering, pause/resume, heartbeats, and tooltip escaping. |
| `test_group_proxy`         | `ConnectionGroupProxy` — Flat mode is strictly pass-through (no parents, no children, 1:1 source mapping → preserves v0.1 view geometry); ByInterface builds expected group/child counts including the "(unattributed)" bucket; ByContainer keys include `runtime` so the same id under docker vs. podman never collapses; SUM aggregation for RxRateRole/TxRateRole/SortRole; mode switching emits modelReset and rebuilds; `sort()` forwards to source in Flat mode and rearranges m_groups + child srcRows in grouped modes (the v0.2-UIUX-C2 regression: header click was a no-op before). **sortWithinGroups** (default true): a header click sorts only each group's children and leaves the group order at first-appearance order; toggling to classic (false) re-orders the groups by aggregated value (and back freezes at the current arrangement) — both via the persistent-index-preserving resort. Uses a tiny stub source model — no real ConnectionModel needed. |
| `test_filter`              | Filter mini-language parser + evaluator (every field/op). v0.4: `pid`, `uid`, `comm`, `runtime`, `container` (multi-haystack across runtime/id/name), `chain_has` (matches any ancestor in `containerChain`). `pid=0` selects unattributed flows by design. v0.5: `reason` (resolved/forwarded/orphaned/nosocket). |
| `test_process_resolver_null` | `qiftop::backend::NullResolver` — pid=0, empty optionals, empty capability list. Smoke test for the universal fallback. |
| `test_resolver_factory`    | `qiftop::backend::createDefaultProcessResolver` — env-gated composite construction; `InterfacesService::capabilities()` aggregation: `process-attribution-wire` / `container-attribution-wire` / `container-chain-wire` mirror tokens emitted iff the underlying resolver advertises the producer-side token; `container-chain-wire` requires BOTH `container-attribution` AND `container-chain`. |
| `test_sockdiag_parse`      | `qiftop::backend::sockDiagParse` — netlink dump message parsing edge cases (IPv4/IPv6, multi-message dumps, truncated tail); plus the local 2-tuple key ambiguity guard (a local key with two distinct inodes must not yield a confident PID). Pure parser, no socket. |
| `test_conntrack_orient`    | `qiftop::backend::linux::orientConntrackFlow` — pure per-flow local/remote orientation + tx/rx-from-ORIG/REPL mapping: outbound (src local), inbound (dst local), forwarded (neither local → ORIG tuple kept), both-local edge. No live conntrack handle. |
| `test_flow_topk`           | `qiftop::backend::linux::admitFlowTopK` (FlowTopK.h) — the bounded top-K-by-bytes min-heap that caps the in-process `ConntrackMonitor` snapshot at the loudest 4096 (mirrors the agent cap): below-cap keeps all, at-cap keeps the loudest regardless of arrival order, ranks by rx+tx total, strictly-greater admission (ties keep the incumbent), `cap<=0` is unbounded, and the min-heap front invariant holds after every op. Pure — no live conntrack handle. |
| `test_services`            | `ConnectionsService` / `InterfacesService` driven in-process via the fake monitors (no real D-Bus bus): snapshot cap (4099→4096 top-by-bytes), dropped-flow skip, process/container/chain attribution, server-side direction; interface-stat caching. |
| `test_cgroup_parse`        | `classifyPathChain` + `classifyPath` synthetic-path coverage of every supported regex (docker systemd + cgroupfs + legacy, containerd, cri-o, podman rootful/rootless, lxd/lxc, nspawn, k3d nested chain, naked k8s cgroupfs/systemd drivers, /user.slice exclusion). Tier-1 regex-shape protection. |
| `test_cgroup_real_fixtures` | Data-driven: 18 real-world `/proc/<pid>/cgroup` fixtures harvested from upstream docs (Docker, containerd CRI, K8s burstable/guaranteed, CRI-O, Podman rootless/rootful, LXD systemd, LXC, systemd-nspawn machinectl/template, host init/session/system-service scopes, /user.slice manager + app under user@<uid>.service). Adding a runtime = drop a fixture + add one table row. |
| `test_proc_snapshot`       | `qiftop::backend::procsnap::pidStartTime` — `/proc/<pid>/stat` field-22 parser robustness (commands with spaces / parens / nested quotes), live self-PID round-trip, missing PID returns nullopt. |
| `attribution_docker` (Tier-2) | Live end-to-end: `runners/run-docker.sh` brings up an alpine container, drives container→host TCP flow, `qiftop-attribution-probe` asks the production resolver chain to attribute the flow back to `runtime=docker` + the right CID prefix. Gated by `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON` (default OFF). |
| `attribution_podman` (Tier-2) | Sibling of `attribution_docker` using rootful podman + netavark; exercises the `libpod-<id>.scope` cgroup hierarchy that the docker path never produces. SKIPs cleanly on hosts where rootful podman can't start a container (e.g. logind rlimit-delegation quirks). |
| `attribution_k3d` (Tier-2) | k3s-in-docker (k3d). Exercises the **nested** container chain (`docker → kubernetes → containerd`, depth 3) — the leaf-wins segment-walk has to land on the innermost containerd CID, not on the outer k3s node container. Local-only (Vagrant); not in CI (cold k3d image pull is ~3–4 min, the chain shape is already pinned by Tier-1 fixtures). |
| `attribution_k8s` (Tier-2) | "Naked" k8s via **k0s --single** (single-binary, no docker wrapper). Distinguishing assertion vs. k3d: chain depth is exactly 2 (`kubernetes → containerd`) and the JSON MUST NOT contain `"runtime":"docker"`. Catches phantom-wrapper bugs in `classifyPathChain`. Local-only (Vagrant). |
| `attribution_systemd_dbus` (Tier-2) | The ONLY runner that exercises the deployed **systemd unit + DBus policy** instead of the in-process probe. Installs the freshly-built `qiftop-agent` component (`cmake --install --component qiftop-agent --prefix /usr`), starts it under systemd, runs a docker container holding ONE long-lived external flow (`sleep 3600 \| nc <host> 22`), then asserts over DBus (`sudo busctl … GetConnections`, JSON parsed in `run-systemd-dbus.sh`) that the flow attributes to `runtime=docker` + CID prefix. Guards the sandbox surface (`RestrictNamespaces`, `CapabilityBoundingSet`) that the probe runners bypass — e.g. the `RestrictNamespaces=yes` regression in §8a rule 5. DESTRUCTIVE / VM-CI-only (needs passwordless sudo + `QIFTOP_BUILD_DIR`). Run via `scripts/local-integration.sh --runtime systemd-dbus`. See §6.5b for the container-flow-generation wisdom it encodes. On the Fedora VM (`--distro fedora`) it also installs the real `.rpm` and audits SELinux AVCs — see §6.5c. |
| `attribution_crio` (Tier-2) | cri-o via `crictl` (sandbox + container JSON, alpine). Generates an external egress flow (`sleep 3600 \| nc <ip> 22`) and reads the in-container tuple via `crictl exec`, then drives the probe expecting `runtime=cri-o` + CID prefix. Note: the classifier only labels the leaf `cri-o` when the host crio.sock probe set `PreferCrio` (otherwise CRI pods read as `containerd`). Gated by `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION`; SKIPs cleanly without cri-o/crictl. |

### 6.3 Gaps worth filling

1. **`ConntrackMonitor::Worker` per-flow diff math** — once extracted
   per §6.4 #3, the diff/accounting logic can be exercised without a
   live conntrack handle.
2. **End-to-end with a real conntrack table** — requires root; needs a
   CI runner with `CAP_NET_ADMIN` or a privileged container.
3. **Tier-2 attribution: more runtimes.** Docker + rootful podman + k3d +
   naked k8s (k0s) runners shipped (`tests/integration/attribution/runners/`).
   Only docker + podman run in CI on push-to-main / dispatch / release —
   k3d/k8s are local-only via the Vagrant harness (cold bring-up is
   several minutes; the chain shapes are pinned by Tier-1 unit fixtures).
   cri-o runner still pending — the probe binary contract is reusable,
   only the bring-up script differs.

### 6.3a Validating against real-world container runtimes

The attribution layer's correctness depends on regex patterns matching
the EXACT paths emitted by each container runtime — and those formats
change between runtime versions, between cgroup drivers (cgroupfs vs.
systemd), and between cgroup v1 vs. v2. Untested regex drift means
silent attribution loss for users running that runtime.

**Tiered validation policy:**

1. **Tier 1 — real /proc fixtures** (`tests/fixtures/cgroup_real/` +
   `test_cgroup_real_fixtures`). Every supported runtime + driver
   combination MUST have at least one fixture sourced from authoritative
   upstream documentation or issue trackers, NOT from this developer's
   imagination. When adding or modifying a regex in `CgroupParse.h`:
     a. Find a real path example in the runtime's upstream docs (e.g.
        docs.docker.com, kubernetes.io, github.com/containers/podman,
        github.com/cri-o/cri-o, github.com/canonical/lxd).
     b. Drop it into `tests/fixtures/cgroup_real/<runtime>.txt`
        verbatim (as the file would appear in /proc).
     c. Add one row to the `classify_data()` data table.
     d. Run the test — RED first (proves the fixture really exercises
        the new regex), then green.

2. **Tier 2 — live container harness** (`tests/integration/attribution/`,
   gated by `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON`). The
   `qiftop-attribution-probe` binary wraps the production resolver
   factory and exposes a CLI for "given this 4-tuple, expect this
   runtime + this CID prefix". Per-runtime runner scripts under
   `runners/` bring up a real container, generate a flow, then drive
   the probe. Currently shipped: `run-docker.sh`, `run-podman.sh`
   (CI), `run-k3d.sh`, `run-k8s.sh` (local-only via Vagrant). Not on
   default ctest. Dev-box driver: `scripts/integration-test.sh
   --runtime docker`.

   **Tier-2 is opportunistic, not mandatory.** Tier-1 fixtures
   protect against regex drift (the high-frequency failure mode);
   Tier-2 mostly re-verifies the NetnsScanner setns(2) plumbing
   that's already generic across every Linux-netns runtime. Add a
   new Tier-2 runner only when the runtime has a GENUINELY-NOVEL
   cgroup/netns shape that the existing runners don't exercise
   (k3d's nested chain qualified; cri-o didn't; nspawn didn't once
   we confirmed the resolver was already name-id safe). See
   docs/ATTRIBUTION.md §5e for the longer rationale. The cost
   ceiling: each new Tier-2 runner adds at least one bridge to the
   test VM and cross-runner state pollution risk grows
   superlinearly (see §6.5a for the k0s/podman case study).

3. **Tier 3 — CI matrix.** Once Tier 2 exists, run it in GitHub Actions
   on every push, matrixed across `docker:latest` / `podman` so
   upstream cgroup-path changes break a PR check, not a user.
   Heavyweight runners (k3d, k8s) stay local-only (cold bring-up
   3-4 min each); their chain shapes are pinned by Tier-1 fixtures.

When in doubt about a real-world cgroup path: consult the runtime's
upstream documentation directly. Recent successful sources used to
build the Tier 1 fixtures:
- docs.docker.com (cgroup drivers, runmetrics)
- kubernetes.io/docs/tasks/administer-cluster/kubelet-cgroup-driver/
- github.com/cri-o/cri-o/blob/main/docs/cgroup.md
- github.com/containers/podman/blob/main/docs/tutorials/rootless_cgroup_v2.md
- github.com/containerd/containerd/blob/main/docs/ops.md
- kernel.org cgroup-v2.html

### 6.4 Refactors that would unblock more testing

1. ~~**Extract `loadIdleConfig` from `src/agent/main.cpp` into
   `agent/Config.{h,cpp}`**~~ **DONE** — see `src/agent/Config.{h,cpp}`,
   covered by `tests/test_agent_config.cpp`.
2. ~~**`main.cpp` glue → `qiftop::agent::Application` class.**~~ **DONE** —
   see `src/agent/Application.{h,cpp}`. RAII: ctor takes
   `QDBusConnection` + the two monitor pointers + `IdleManager::Config`;
   `start()` registers objects, requests the bus name, wires the
   IdleManager, starts monitors. Dtor unregisters + releases the name so
   the same bus can be re-used by another `Application` instance (useful
   for in-process integration tests). `main.cpp` is now a ~85-line shim.
3. **Extract `ConntrackMonitor::Worker`'s per-flow diff math** into a
   free function `computeDeltas(prev, current) -> QList<Connection>` so
   the diff/accounting tests don't need a live conntrack handle.
4. **Add `tests/fakes/FakeNetworkMonitor.{h,cpp}`** emitting canned
   `statsUpdated` signals on demand, to drive `IdleManager` and the
   services from tests.

### 6.5 CI

`.github/workflows/ci.yml` runs the full test suite under
`dbus-run-session` with `QT_QPA_PLATFORM=offscreen` on a matrix of
native ubuntu-24.04 × Debug + Release, plus a containerised matrix
(ubuntu:26.04 + fedora:44) × Debug + Release. The 26.04 slot is
containerised pending availability of a native ubuntu-26.04 GitHub
Actions runner image. `HOME` is redirected to
`$RUNNER_TEMP/home` so QSettings/Autostart tests don't trample the
runner user. See docs/HACKING.md §5.5 for the test-writing conventions.

`.github/workflows/clang-tidy.yml` is a Phase-0 static-analysis report:
non-gating (`continue-on-error`), configure/autogen-only, and routed through
`scripts/run-clang-tidy.sh` so PRs get signal without surprise red lights.

`.github/workflows/integration.yml` runs the Tier-2 attribution
runners (docker + podman only) on push-to-main / dispatch / release.
k3d and k8s runners exist but only run locally via the Vagrant
harness — cold bring-up is 3–4 min each and the chain shapes are
already pinned by Tier-1 unit tests
(`test_cgroup_parse::k8sCgroupfsDriverK3dShape`,
`k8sNakedCgroupfsDriver`, `k8sNakedSystemdDriver`).

### 6.5a Vagrant runner ordering (local Tier-2)

The Vagrant VM at `tests/integration/vagrant/` (the `ubuntu` machine —
see §6.5c for the `fedora` one) is the supported home for the heavyweight
runners (k3d, naked k8s). One sharp edge to remember when iterating:

* **k0s bundles kube-router**, which inserts persistent rules into
  the host's `INPUT` (`KUBE-FIREWALL`, `KUBE-ROUTER-INPUT`) and
  `FORWARD` chains and leaves zombie veth devices on `cni-podman0` /
  `docker0`. These survive `systemctl stop k0scontroller`.
* Symptom: `attribution_podman` fails with `ss did not observe an
  inbound flow on :18081` even though `podman run ... alpine` works
  for trivial cases — packets from the container never reach the
  host bridge because kube-router has hijacked forwarding.
* ctest orders the runners by test number: 23 docker → 24 podman →
  25 k3d → 26 k8s, so a single cold-boot run is fine. If you re-run
  podman after k8s/k3d started on the same boot, **`vagrant reload
  --no-provision` first** (or destroy + recreate). Cheaper than
  fighting the rules. Don't be tempted to flush iptables manually —
  netavark's chain references go stale and you'll spend an hour
  debugging zombie veths.

### 6.5b The systemd + DBus runner, and container-flow generation wisdom

`tests/integration/attribution/runners/run-systemd-dbus.sh`
(ctest: `attribution_systemd_dbus`, label `attribution-integration`) is
the **only** test that exercises the agent the way users actually run it:
installed as a systemd unit, sandboxed, queried over the system DBus.
Every other attribution runner drives `qiftop-attribution-probe`, which
builds the resolver chain *in-process* as root with no sandbox — so it
structurally cannot catch regressions in the unit's hardening
directives, capability set, or the DBus policy. The motivating bug:
`RestrictNamespaces=yes` silently seccomp-blocked `setns(CLONE_NEWNET)`
so the systemd agent attributed every container flow as host, while the
probe passed (§8a rule 5). The runner installs the freshly-built
`qiftop-agent` component with `cmake --install --component qiftop-agent
--prefix /usr` (the `/usr` prefix is mandatory — the unit's
`ExecStart=/usr/bin/qiftop-agent` is absolute and won't pick up a
`/usr/local` install), restarts the unit, runs a container, and asserts
attribution over `busctl ... GetConnections`. DESTRUCTIVE + needs
passwordless sudo ⇒ VM/CI-only; not on default ctest. Validate in the
Vagrant VM (`scripts/local-integration.sh --runtime systemd-dbus`), the
authoritative venue — not on a dev box.

**Generating a container flow the agent will ATTRIBUTE is the hard part,
and most "obvious" recipes silently fail.** Attribution needs a flow
that is BOTH (a) tracked in the host conntrack table AND (b) backed by a
*live socket* at query time (the NetnsScanner attributes via the owning
process's socket; a conntrack husk with no live socket is unattributable
by design). The traps, all hit during this work:

* **container → docker0 / any host-local IP** — delivered locally
  (INPUT), so docker's `MASQUERADE` (which only fires `! -o docker0`)
  never tracks it. Not in conntrack ⇒ the agent never sees it.
* **container → container on the same bridge** — L2-switched, never
  hits the netfilter forward path ⇒ not conntracked.
* **busybox `wget https://…`** (alpine default) — no TLS support, so the
  download never happens; the container silently falls through to its
  fallback (`sleep`) with *no socket at all*. Use a plain-TCP generator
  or install `ca-certificates`.
* **`nc host port` in a reconnect loop** — each iteration is a new PID,
  so by the time the data thread resolves the flow the socket-owning PID
  has changed; the PID-reuse `starttime` guard (§8a rule 2) correctly
  rejects it ⇒ `pid=0`, unattributed.
* **detached `nc host port` with closed stdin** — busybox nc EOFs
  immediately and exits; the container dies or the socket is gone.

The recipe that works: **`sleep 3600 | nc <external-host> 22`** — the
`sleep` keeps nc's stdin open so the single, stable-PID nc process holds
ONE connection, and an SSH server holds the pre-auth connection for its
LoginGraceTime (~120 s), far longer than the test. External ⇒
masqueraded ⇒ conntracked; single long-lived process ⇒ stable socket ⇒
attributable. Resolve the target to an IP on the host and dial the IP
(not the name) to dodge busybox DNS flakiness and to pin the remote so
stale husks to other round-robin IPs can't be mistaken for the flow.

Two more sharp edges the runner encodes:

* **`GetConnections` returns the cached `m_last`**, refreshed only on a
  poll tick (`src/agent/ConnectionsService.cpp`). You MUST send a
  `SetDesiredIntervalMs` cadence hint AND wait a tick before reading, or
  you get a stale snapshot that predates your flow. The runner warms
  *then* sleeps *then* reads.
* **`busctl --json=short | python3 - <<'PY'` is a stdin trap**: the
  heredoc IS python's stdin (`python3 -` reads the program from stdin),
  so a piped JSON never reaches `sys.stdin`. Write the JSON to a temp
  file and pass the path as `argv`.

Diagnostics gotcha: the **`conntrack -L` CLI can disagree with the
agent's `libnetfilter_conntrack` dump** on a busy host (we saw the CLI
return nothing for docker flows the agent happily reported). Trust the
agent's `GetConnections`, not the CLI, when deciding whether a flow is
visible. Also note that **hammering the agent with many rapid
restarts degrades conntrack/netns state** enough to make attribution
flap — another reason this runner belongs in a disposable VM, one clean
boot per run.

Two portability traps the runner hit when first validated in the VM (and
now encodes/works around):

* **Container IPv4 lives under `.NetworkSettings.Networks.<net>.IPAddress`
  on newer docker / podman-docker**, NOT the legacy top-level
  `.NetworkSettings.IPAddress` (which is empty/absent there). The runner
  walks the `Networks` map first, then falls back to the top-level field.
  The devbox's older docker populated the top-level field, so this only
  surfaced in the VM.
* **`qiftop-agent` has a `POST_BUILD` cpack hook that regenerates BOTH
  `.deb`s**, which needs the `qiftop` GUI binary on disk. Under Ninja's
  parallel scheduling the hook can fire the moment the agent links —
  before the GUI target finishes — so a fresh `cmake --build --target
  qiftop-agent` fails with `file INSTALL cannot find …/qiftop`. Both
  driver scripts therefore build the **`qiftop` GUI target to completion
  FIRST**, then the agent + probe. (On an incremental tree where the GUI
  already exists, as on the devbox, the ordering is moot — which is why
  this only bit on the VM's clean build.)

### 6.5c The Fedora SELinux VM

The Vagrant harness is **multi-machine**: `ubuntu` (primary, full runner
set) and `fedora` (opt-in, `autostart: false`). The Fedora VM exists for
the two things the Ubuntu VM structurally cannot cover:

1. **SELinux.** Ubuntu uses AppArmor with permissive-ish profiles;
   Fedora ships SELinux **enforcing**. The agent does exactly the things
   SELinux likes to deny on a sandboxed root service — `setns(CLONE_NEWNET)`,
   `NETLINK_SOCK_DIAG` sockets, cross-netns `/proc/<pid>/{ns,cgroup}`
   reads. The probe runners (in-process, no sandbox) cannot catch an
   SELinux denial; only the deployed systemd unit can.
2. **The real `.rpm` install path.** The Fedora path installs the agent
   from the freshly-built `.rpm` (`dnf remove` + `install`, exercising
   both scriptlets and `restorecon` file labels) instead of
   `cmake --install`.

Run it with `scripts/local-integration.sh --distro fedora` (defaults to
the `systemd-dbus` runner; the Fedora VM has no k3d/k8s provisioned).
The driver additionally `cpack -G RPM`s in the guest and exports
`QIFTOP_AGENT_RPM_DIR` + `QIFTOP_SELINUX_AUDIT=1`, forwarded through
`sudo --preserve-env` so the runner picks them up.

`run-systemd-dbus.sh` honours those two env vars (off by default, so the
Ubuntu path is byte-for-byte unchanged):

* `QIFTOP_AGENT_RPM_DIR=<dir>` → install the agent from `<dir>/qiftop-agent-*.rpm`.
* `QIFTOP_SELINUX_AUDIT=1` → after the attribution assertion, `ausearch
  -m avc` for **qiftop** denials since the agent started. Under the
  default `unconfined_service_t` domain (we ship no SELinux policy
  module yet) there should be **zero** — so a hit means the agent
  tripped SELinux. A clean run is the reassuring answer ("qiftop runs
  fine under Fedora targeted policy"); a denial is the signal that a
  policy module is needed before the agent can run *confined*. Denials
  are fatal to the test (`exit 1`) and are also dumped on the
  FOUND-UNATTRIBUTED failure path (where a setns denial would be the
  smoking gun, mirroring the seccomp `RestrictNamespaces` regression in
  §8a rule 5).

SELinux enforcing is inherently a **VM-only** thing — GitHub Actions
containers share the host kernel and can't `setenforce`, so there is no
CI job for this; validate in the Fedora VM. The box defaults to
`generic/fedora41` (good rsync/libvirt compat) and is overridable via
`QIFTOP_FEDORA_BOX`. Multi-machine note: the machines are now named
`ubuntu` / `fedora`; an old unnamed `default` VM from before this split
can be dropped with `vagrant destroy default`.

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
* **Per-column visibility / order / width is persisted via
  `QHeaderView::saveState()` / `restoreState()`** under the
  `ui/interfaces/headerState` and `ui/connections/headerState` keys.
  The opaque blob already covers visibility + visual order + widths +
  sort indicator — don't add parallel per-column-name settings keys.
  The user-facing toggle UI lives in the header right-click menu
  (`MainWindow::showNetHeaderMenu` / `showConnHeaderMenu`) and the menu
  refuses to hide the last visible column. `applySettingsToUi()` still
  force-toggles `RxMax` / `TxMax` based on the throughput-gauge
  setting — those two columns are gauge-dependent, not user-controlled.
* **Connections view is a QTreeView with a two-proxy chain**:
  `ConnectionModel` → `ConnectionFilterProxy` → `ConnectionGroupProxy`
  → `QTreeView`. Flat mode is the default and is a strict pass-through
  in `ConnectionGroupProxy` (top-level rows map 1:1 to source rows; no
  parents, no children, `internalId == quintptr(-1)`); the view also
  flips `setRootIsDecorated(false)` + `setIndentation(0)` + disables
  expand-on-double-click so RowGaugeDelegate / ConnectionFlowDelegate
  paint at the same coordinates as the v0.1 QTableView. The grouped
  modes (`ByInterface` / `ByContainer` / `ByProcess`) synthesize a
  one-level tree where group rows aggregate per-column SUMs via
  `aggregateData()` and child rows forward to source via
  `mapToSource()`. **When mapping back to the source model, always
  walk both proxies** (`m_connGroupProxy->mapToSource(view)` then
  `m_connProxy->mapToSource(filter)`) and skip group rows via
  `m_connGroupProxy->isGroupIndex(view)`. Adding a new grouping mode
  = extend the `Settings::ConnectionViewMode` enum + `groupKeyFor()`
  + `groupLabelFor()`; the rest is automatic. **Header-click sort
  behaviour is gated by `Settings::sortWithinGroups`** (default true):
  when set, a header click sorts only each group's child rows and keeps
  the group order frozen at first-appearance order; when cleared, the
  classic global sort orders the groups by aggregated value too.
  `applySettingsToUi()` pushes it via
  `ConnectionGroupProxy::setSortWithinGroups()`, which re-sorts with
  full persistent-index preservation (no model reset → expansion/
  selection survive).
* **Process / Container columns are capability-gated, on by default.**
  `Column::Process` and `Column::Container` default to *shown*
  (`Settings::showProcessColumn` / `showContainerColumn` default `true`)
  but are AND-gated on the agent's matching wire token, so they only
  appear when attribution data is actually available; the data is
  already on the wire, so showing them costs nothing. The Settings
  dialog's "Process & Container Attribution" sub-section
  (Display tab) advertises three toggles — `Settings::showProcessColumn`
  / `showContainerColumn` / `showContainerChainInTooltip` — each
  effective only when the agent advertises the matching wire token
  (`process-attribution-wire` / `container-attribution-wire` /
  `container-chain-wire`). The values persist regardless so they take
  effect when the user later runs against an attribution-capable
  agent. `applySettingsToUi()` is the single point where the
  user-pref AND the wire-token are AND-ed together to (un)hide each
  column — never `setColumnHidden()` either column outside that
  helper. The same helper ALSO suppresses the column that the active
  grouping makes redundant: grouping `ByProcess` hides the Process
  column and `ByContainer` hides the Container column (the value lives
  in the group header), so the AND-gate is
  `wireToken && userPref && !groupedByThatKey`. Changing the grouping
  re-runs `applySettingsToUi()` (via `Settings::changed`), so the
  column hides/restores live. The header right-click menu's Process / Container entries
  are routed through `Settings::setShowProcessColumn` /
  `setShowContainerColumn` (the menu interceptor at
  `MainWindow::showConnHeaderMenu`), so toggling them updates the
  persisted setting AND triggers `applySettingsToUi()` — the AND-gate
  still applies. Without the wire token a header-menu toggle persists
  the user's preference but the column remains hidden until a
  capable agent is detected; the checkbox in the menu reflects the
  ACTUAL visibility (it stays unchecked), not the persisted setting.
  Pinned end-to-end by
  `test_mainwindow_smoke::processColumnHiddenWithoutWireCapability`.
  The `nqiftop` TUI reaches the same parity by construction — it has no
  Process/Container columns, so a grouped child row renders only the flow
  (`cellsForConnection` = proto + endpoints) while the group header
  (`groupLabelFor`) carries the grouped attribute; nothing is repeated
  per row. Guarded by
  `test_tui_format::groupedChildRowCarriesNoRedundantAttribution`.
* **Privilege escalation env handling.** `src/util/PrivilegeEscalator.cpp`
  uses an **allowlist** (`sessionEnv()`) when forwarding environment
  variables into the privileged child, not a denylist. The root child runs
  with `AT_SECURE` unset, so ld.so honours `LD_PRELOAD` / `LD_LIBRARY_PATH`
  / `LD_AUDIT` / `QT_PLUGIN_PATH` etc. from whatever environment it sees;
  a denylist would always lose this race against new loader knobs. If a
  helper needs a new env var to work, add it to `kAllow` explicitly and
  audit whether an attacker could weaponise it. The same allowlist is
  applied to every QProcess we spawn (not just pkexec) via
  `scrubbedHelperEnv()` so that kdesu / gksudo / lxqt-sudo / beesu can't
  pass through hostile env vars either. **`PATH` is deliberately NOT on
  the allowlist** — both `sessionEnv()` and `scrubbedHelperEnv()` force
  `PATH=/usr/sbin:/usr/bin:/sbin:/bin` to keep the privileged child safe
  from any future relative-path exec resolving through a user-controlled
  directory (LPE primitive). Pinned by `test_priv_escalator`.
* **Settings persistence under elevation.** Because `sessionEnv()`
  forwards `HOME` (so the elevated child finds the user's theme/font
  config), and because `sudo -E` / some `su`/`pkexec` setups also
  preserve `HOME`, a privileged qiftop/nqiftop process resolves
  `QSettings` to the *invoking* user's `~/.config` — writing there would
  litter their home with **root-owned** `.conf` files (a recurring
  papercut). `qiftop::platform::settingsWriteWouldEscalate()` returns
  true when `geteuid()==0` and the config dir's nearest existing ancestor
  is owned by a non-root user; both the GUI `Settings` (`m_persist`
  gate on `store()` + the legacy migration) and the TUI
  `TuiApp::saveSettings()` then **load but never write** settings. Normal
  unprivileged runs persist exactly as before. Pinned by
  `test_settings_migration::unprivileged_persistence_unaffected`.
* **Handoff IPC hardening.** `HandoffServer` (parent ↔ privileged child
  IPC) enforces several invariants worth keeping intact:
    - Socket lives under `$XDG_RUNTIME_DIR` (mode 0700, kernel-managed)
      or, when that's missing, a freshly `mkdtemp`'d 0700 directory under
      `$HOME/.cache/qiftop/handoff-XXXXXX/`. **Never `/tmp`** — `bind()`
      in a world-writable directory leaves a permission-race window.
    - Every accepted peer must pass `SO_PEERCRED` (uid matches parent
      or is 0). Defence-in-depth on top of the 0600 socket mode.
    - The 256-bit auth nonce is written to a 0600 file and the *path*
      is forwarded via `QIFTOP_HANDOFF_NONCE_FILE`. **Never put the
      nonce on argv** — `/proc/<pid>/cmdline` is world-readable for the
      lifetime of the pkexec prompt. The child reads + unlinks
      immediately.
    - An unauthenticated incumbent peer is evicted in favour of a
      newcomer, so a same-uid process can't camp the slot pre-HELLO.
      Once authenticated the slot is sticky.
    - Read buffers are capped: 1 KiB pre-auth, 1 MiB post-auth.
    - Pinned by `test_handoff_auth`.
* **CSV / spreadsheet injection.** `src/util/Exporter.cpp::csvSanitise`
  prepends a leading apostrophe to any field starting with `=`, `+`, `-`,
  `@`, `\t`, or `\r` before quoting. Attacker-controlled hostnames (via
  reverse DNS) or interface names from the kernel must not be able to
  execute as spreadsheet formulas when the user opens an exported CSV.
* **`IdleManager::setClientHint` returns `bool`.** Services should only
  call `noteActivity()` on accepted hints — otherwise a peer rejected
  from the (capped) hint table can still pin the agent out of idle by
  hammering `SetDesiredIntervalMs`. Pinned by `test_idle`.

---

## 8. Release / versioning

* `project(qiftop VERSION ...)` in `CMakeLists.txt` is the single source of
  truth. Component package versions, `CPACK_PACKAGE_VERSION`, and the
  binaries' runtime `--version` (via the `QIFTOP_VERSION` compile definition)
  derive from it.
  CPack emits one package per component: `qiftop`, `qiftop-agent`,
  `nqiftop` when built, and the libqiftop runtime/development packages
  (`libqiftop0` / `libqiftop-dev` for DEB, `qiftop-libs` /
  `qiftop-devel` for RPM).
* The DBus interface name (`org.qiftop.NetworkAgent1`) carries the
  contract version. **If you make a breaking change to a DTO or to a
  method signature, bump to `NetworkAgent2` and keep the old one alive
  for one release.**
* User-facing config (`/etc/qiftop/agent.conf`) is a conffile — additions
  are fine, removals/renames are not. Always keep loader code tolerant
  of unknown keys.
* **Debug symbols: pre-release vs final.** The release workflow keys off
  the tag shape (a `-` suffix like `v0.2-rc1` ⇒ pre-release). Pre-release
  builds are `RelWithDebInfo` + `CPACK_STRIP_FILES=OFF` so user crash
  backtraces are debuggable; final builds are `Release` + stripped.
  Symbols are kept **inline** in the main package (no separate
  `-dbgsym`/`-debuginfo` packages) for BOTH formats: the `.deb` is simply
  left unstripped, and the `.rpm` matches via a `CPACK_STRIP_FILES`-gated
  block in `CMakeLists.txt` that sets `debug_package %{nil}` +
  `__strip /bin/true` to neutralise rpmbuild's brp-strip / debuginfo
  split. Both jobs must pass `-DCPACK_STRIP_FILES=${strip_files}`.
* **No source packages.** CPack emits binary `.deb`/`.rpm` only — there
  is no Debian source package (`.dsc`/`Sources` index) or SRPM, so
  `apt source qiftop` / `dnf download --source qiftop` do not work. The
  source is the git repo (clean `cmake` build). Adding `deb-src`/SRPM
  would need real Debian packaging (`debian/`) or an SRPM repo and is
  low-ROI for a from-source-buildable project.

### Package distribution (apt / dnf repos)

* Signed **apt** + **dnf** repos are hosted on **GitHub Pages** at
  `https://thecleaners.github.io/qiftop/`. GitHub Packages (ghcr)
  cannot serve apt/dnf, and the `TheCleaners` org has Pages creation
  behind an org setting — it must stay enabled for deploys to work.
* `.github/workflows/pages.yml` (triggers: `workflow_dispatch` +
  `release: published`) downloads every release's `.deb`/`.rpm` assets
  and runs `dist/repo/build-pages.sh` (`apt-ftparchive` + `createrepo_c`),
  then deploys via `actions/deploy-pages`. The package files live as
  **release assets, never in git** — do not commit `.deb`/`.rpm` blobs
  (keeps clones lean, same reason we purged the demo gif).
* **GITHUB_TOKEN event gotcha:** a release created by the Release
  workflow's token does NOT fire `release: published` (recursion
  guard), so the first publish after a tag-driven release needs a
  manual `gh workflow run pages.yml`. Manual UI/PAT releases trigger it
  automatically.
* **Signing:** repos are GPG-signed by the project key
  `7AC658ABFADD1AAF6E0EDA6F6DD33D47032BD42D` ("qiftop package
  signing"). The private half lives in the `GPG_PRIVATE_KEY` Actions
  secret (passphraseless); the public half is committed at
  `dist/repo/qiftop-archive-keyring.asc` and published at the Pages
  root. The dnf repo signs BOTH the metadata and each package: the
  detached `repomd.xml.asc` (`repo_gpgcheck=1`) authenticates package
  checksums (the analog of apt's signed `Release`), and every `.rpm` is
  individually signed with `rpm --addsign` (header RSA/SHA256,
  `gpgcheck=1`). Headless signing on the Ubuntu pages runner needs an
  explicit `__gpg_sign_cmd` macro (rpm's default invocation omits
  `--batch`/`--pinentry-mode loopback`, so a passphraseless key fails
  with "gpg exec failed") — see `dist/repo/build-pages.sh`. If
  `GPG_PRIVATE_KEY` is absent the workflow publishes unsigned
  (`gpgcheck=0 repo_gpgcheck=0`, forks).

---

## 8a. Lifetime & races in process / container attribution

Process and container attribution (`ProcessResolver` and its
implementations under `src/backend/linux/`) chase information that the
kernel can invalidate at any moment: processes exit, fds close, cgroups
get removed, network namespaces get destroyed. Every resolver MUST treat
those as the normal case and degrade silently rather than crash, spam
the log, or — worst — return *wrong* attribution.

Concrete rules every resolver must follow:

1. **All `/proc/<pid>/*` reads tolerate ENOENT / EACCES.** Any open or
   readlink that races a vanishing process returns `nullopt`, not an
   error. No `qWarning` on these — they happen every second on a busy
   host and would drown the log. (See SockDiagResolver's
   `refreshProcWalk`: `opendir`/`readlink` failures just `continue`.)

2. **PID reuse is guarded by starttime.** PIDs wrap on busy hosts (and
   on hosts with `kernel.pid_max=32768` they wrap fast). Any cache or
   late-bound lookup that maps `pid → info` MUST snapshot
   `/proc/<pid>/stat` field 22 (starttime in jiffies-since-boot) at
   enrollment time and re-check on use. A mismatch means the kernel has
   handed that pid to a different process; the cached attribution is
   stale and MUST be discarded, not served. See
   `backend/linux/ProcSnapshot.h::pidStartTime` and its use in
   `SockDiagResolver::resolveFlow` + `CgroupClassifier::resolveContainerForPid`.

3. **Cached "info is unknown" is fine.** When the underlying file
   disappears, caching `nullopt` keyed by (pid, starttime) for the TTL
   is correct: short-lived processes don't deserve repeated /proc
   churn, and the entry will age out naturally.

4. **Container names are display strings, not handles.** A
   `ContainerInfo{runtime, id, name}` returned to the GUI may refer to
   a cgroup that has since been deleted (container stopped). That's
   fine — we display the snapshot we took. Never re-resolve "live" from
   a cached id without going back through the resolver chain (which
   re-snapshots starttime + cgroup path).

5. **Namespace operations (step 4 — NetnsScanner) must be RAII-fenced.**
   The pattern, when implemented:
     - Open `/proc/<pid>/ns/net` with `O_RDONLY | O_CLOEXEC`. ENOENT →
       skip this pid silently.
     - `fstat()` the fd; the `st_ino` uniquely identifies the netns.
       Dedupe scans by inode so multiple pids in the same container
       cost one dump.
     - Capture the worker thread's *original* netns fd FIRST. Wrap the
       `setns()` + dump + `setns(back)` sequence in a scope guard whose
       destructor restores the original netns. **If the restore
       `setns()` ever fails, `qFatal`** — we're stuck running future
       sock_diag dumps in the wrong netns and would attribute every
       host flow to the wrong container.
     - The dump itself running in a since-destroyed netns returns
       ENETUNREACH or empty results; treat as "no flows", not an error.
     - All of this happens on the agent's worker thread so the main
       thread is never affected.
     - **systemd sandbox gotcha:** `setns(CLONE_NEWNET)` is silently
       blocked by `RestrictNamespaces=yes` (it seccomp-denies *all*
       namespace ops). The unit MUST use `RestrictNamespaces=net`
       (allow only the net type) or every container-side flow
       attributes as host with no error in the log — the scanner
       enters the netns loop, `setns()` returns EPERM, the per-netns
       socket dump is skipped, and the merged map stays empty. This
       bit us live (a `dockurr/tor` container showed no grouping);
       the symptom is `netns-scan` advertised + "refreshed across N
       netns" logged, yet zero container attribution. It also needs
       `CAP_SYS_ADMIN` (granted) — both are required.

6. **No `int`-returning syscalls without checking errno.** Anything
   that can fail (socket, bind, sendto, recv, opendir, setns,
   readlink, openat) must either bail out cleanly or log via
   `qCWarning(lcVerbose)` exactly ONCE per cycle — not per failure.

7. **Resolver methods must be safe to call from the data-emission
   loop.** That loop has a per-tick budget; resolvers are NOT allowed
   to do unbounded I/O or block on external services. The `kCacheTtlMs`
   constants in each resolver are the contract: refresh costs amortise
   to roughly once per TTL per pid, never per flow.

8. **Bounded caches, hard cap, clear-on-overflow.** Every per-pid cache
   has an upper bound (e.g. `CgroupClassifier::kCacheMaxItems = 8192`).
   On overflow, full-clear rather than LRU-evict — at this scale the
   complexity isn't justified and the next tick repopulates the hot
   set. **Never** let the cache grow unbounded; a host with 100 k
   short-lived workers per minute would OOM the agent otherwise.

Unit coverage of the race surface lives in `tests/test_proc_snapshot.cpp`
(parser robustness + live self-pid stability) and the existing resolver
unit tests; for end-to-end "actually races a dying process" coverage we
rely on the integration test running under `dbus-run-session`.

When adding a new resolver: re-read this section, then ask yourself
"what happens if every /proc/<pid>/* file I touch returns ENOENT?" If
the answer is anything other than "the resolver returns nullopt and the
caller carries on", the design is wrong.

---

## 9. Keeping this document fresh

This file describes the *current* state of the codebase — architecture,
contracts, layering rules, conventions. It is **not** a changelog;
`git log` is. Earlier revisions of AGENTS.md kept a hand-maintained
list of dated entries at the bottom, which inevitably drifted out of
sync with reality. That list has been removed; use `git log -- AGENTS.md`
(or `git log -p -- AGENTS.md`) to see how the doc evolved.

When you make a non-trivial change to the codebase:

* Update the relevant section above so it still describes how the code
  actually works. Same commit as the code change.
* If the change affects a dev-loop recipe (build, run, debug, package,
  test), update [docs/HACKING.md](docs/HACKING.md) too.
* Don't add a dated entry here — the commit message and `git log` are
  the canonical record.

Things that almost always need an AGENTS.md edit:

* New or renamed DBus method / signal / object path → §4.
* New config key → §5 (and `dist/conf/agent.conf`).
* New backend or platform → §2 + §7's "Adding a New Platform Backend".
* New layering boundary or violation fix → §2 "Layering rules".
* New test or test infrastructure → §6.
* Breaking change to a public type or the wire format → §8 (bump
  contract version), plus the relevant DTO docs in §4.
