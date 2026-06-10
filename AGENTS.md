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
│   ├── Attribution.{h,cpp}  # pure helper: enrich Connection list via ProcessResolver
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
5. **`util/`, `dbus/`, and `backend/` (excluding `backend/dbus/` which is
   client-side) must not include from `ui/` or depend on Qt Widgets.**
   These directories — plus the pure-logic headers under `ui/` like
   `util/ConnectionHeuristics.h` — are the future `libqiftop` material
   (see §10). Anything pulling in `QWidget`, `QAbstract*Model`,
   `QSortFilterProxyModel`, or similar belongs in `ui/`.
6. **No platform headers outside `backend/<os>/` or `backend/PlatformInfo.cpp`.**
   `<linux/*>`, `<sys/un.h>`, `<netinet/*>`, `<ifaddrs.h>`, `<windows.h>`,
   `<sys/sysctl.h>` etc. live in the platform subdirectory's translation
   units, or behind `qiftop::platform` (with documented fallbacks on
   unsupported OSes). Code in `agent/`, `ui/`, `dns/`, `config/`,
   `dbus/` and the abstract `backend/*.h` interfaces must compile on
   any Qt 6 target. See `docs/PORTABILITY.md` for the full survey of
   what each target OS would need.

### Future direction (long-term)

The current binary split (`qiftop` GUI + `qiftop-agent` daemon) is
expected to grow a third component: **`libqiftop`**, a Qt6::Core-only
shared library carrying the DTOs, aggregation/EMA helpers, filter
mini-language, and unit formatters. Planned consumers beyond the Qt
GUI: a Prometheus-style metrics exporter, an alerting daemon, and
possibly an ncurses frontend. This is **not v0.1 work**, but two
things follow now:

* The DBus contract is effectively a public ABI for those future
  consumers — treat DTO breakage as a multi-frontend cost, not just
  a GUI cost.
* Resist any change that tightens Widgets coupling in `util/`,
  `dbus/`, `backend/`, or the pure-logic headers under `ui/`. If you
  catch yourself reaching for `QAbstractItemModel` in a utility,
  that's a smell — the model should wrap a plain `QObject`
  aggregator, not be the aggregator.

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

**Connection wire signature** (22 outer fields + nested chain):
`a(yysqysqttttsyuyuussssa(sss))` =
`(proto, localFamily, localAddress, localPort, remoteFamily, remoteAddress,
remotePort, rxBytes, txBytes, rxPackets, txPackets, iface, direction,
ifIndex, tcpState, pid, uid, comm, containerRuntime, containerId,
containerName, containerChain[a(sss)])`.

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
  socket couldn't be located via SOCK_DIAG within the cache window).
  `comm` is the kernel-truncated 15-byte basename. Expensive fields
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

### Contract version & capabilities

`Version` is a free-form string (currently `"0.5"`) bumped only for
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
> change). Since only pre-release alphas existed in those windows we
> reshaped in place rather than branching `NetworkAgent2`. Older
> alpha clients failing to unmarshal fall back cleanly to the
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
* **`GetProcessDetails` exposure tradeoff.** The on-demand RPC returns
  `comm`/`exe`/`cmdline`/`cwd` for any reachable PID. `/proc/<pid>/exe`
  and `/proc/<pid>/cwd` symlinks are normally mode-0700 (readable
  only by the process owner and root), and `/proc/<pid>/cmdline` is
  world-readable but can leak credentials passed on the command line.
  The root agent reads these and ships them to any netdev-group caller.
  We accept this as trust-equivalent on top of the existing bulk
  pid/uid/comm exposure in `GetConnections` — netdev membership is
  already a "network admin" capability. Distros that want a stricter
  gate should narrow `GetProcessDetails` in the bus policy file rather
  than disabling the whole interface.

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

### 6.2 What's currently covered

| Test                       | Subject                                                           |
|----------------------------|-------------------------------------------------------------------|
| `test_direction`           | `inferDirection` (ephemeral-port + local-end fallback)            |
| `test_forwarded`           | `isForwardedFlow` heuristic                                       |
| `test_ema`                 | `emaUpdate`, `easeOutCubic`                                       |
| `test_settings_migration`  | `Settings` legacy-key migration logic                             |
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
| `test_dbus_types`          | `ConnectionDto` wire round-trip: IANA proto mapping, direction field, out-of-range direction clamp; v0.4 attribution round-trip + defaults. |
| `test_attribution`         | `agent::attributeFlows` — null-resolver no-op, process-only / container-only / chain attribution paths, per-PID memoisation (50 flows from same PID = 1 container lookup), chain opt-in obeys `wantContainerChain` flag, flow without PID never triggers `resolveContainerForPid(0)`. Uses a FakeResolver — no /proc, no sock_diag. |
| `test_composite_resolver`  | `qiftop::backend::CompositeResolver` — empty composite is a no-op; first-non-nullopt fan-out for resolvePid / enrichPid / resolveContainerForPid; capability tokens are unioned and de-duplicated in first-seen order; `resolveContainerChainForPid` deliberately bypasses the base-class single-wrap fallback so chain-capable children get to provide the real OUTER→INNER ancestry; initialize() probes EVERY child (not short-circuited). Uses a programmable FakeResolver — no Qt Widgets, no DBus. |
| `test_proc_details`        | `readProcessDetails` (Linux on-demand RPC backend) — invalid/missing PID returns `valid=false` without crashing; self-PID round-trips pid/uid/cmdline/exe; `/proc/<pid>/stat` field-22 starttime parser is non-zero; alternate procRoot parameter is honoured (fixtureability seam). |
| `test_group_proxy`         | `ConnectionGroupProxy` — Flat mode is strictly pass-through (no parents, no children, 1:1 source mapping → preserves v0.1 view geometry); ByInterface builds expected group/child counts including the "(unattributed)" bucket; ByContainer keys include `runtime` so the same id under docker vs. podman never collapses; SUM aggregation for RxRateRole/TxRateRole/SortRole; mode switching emits modelReset and rebuilds; `sort()` forwards to source in Flat mode and rearranges m_groups + child srcRows in grouped modes (the v0.2-UIUX-C2 regression: header click was a no-op before). Uses a tiny stub source model — no real ConnectionModel needed. |
| `test_filter`              | Filter mini-language parser + evaluator (every field/op). v0.4: `pid`, `uid`, `comm`, `runtime`, `container` (multi-haystack across runtime/id/name), `chain_has` (matches any ancestor in `containerChain`). `pid=0` selects unattributed flows by design. |
| `test_process_resolver_null` | `qiftop::backend::NullResolver` — pid=0, empty optionals, empty capability list. Smoke test for the universal fallback. |
| `test_resolver_factory`    | `qiftop::backend::createDefaultProcessResolver` — env-gated composite construction; `InterfacesService::capabilities()` aggregation: `process-attribution-wire` / `container-attribution-wire` / `container-chain-wire` mirror tokens emitted iff the underlying resolver advertises the producer-side token; `container-chain-wire` requires BOTH `container-attribution` AND `container-chain`. |
| `test_sockdiag_parse`      | `qiftop::backend::sockDiagParse` — netlink dump message parsing edge cases (IPv4/IPv6, multi-message dumps, truncated tail). Pure parser, no socket. |
| `test_cgroup_parse`        | `classifyPathChain` + `classifyPath` synthetic-path coverage of every supported regex (docker systemd + cgroupfs + legacy, containerd, cri-o, podman rootful/rootless, lxd/lxc, nspawn, k3d nested chain, naked k8s cgroupfs/systemd drivers, /user.slice exclusion). Tier-1 regex-shape protection. |
| `test_cgroup_real_fixtures` | Data-driven: 18 real-world `/proc/<pid>/cgroup` fixtures harvested from upstream docs (Docker, containerd CRI, K8s burstable/guaranteed, CRI-O, Podman rootless/rootful, LXD systemd, LXC, systemd-nspawn machinectl/template, host init/session/system-service scopes, /user.slice manager + app under user@<uid>.service). Adding a runtime = drop a fixture + add one table row. |
| `test_proc_snapshot`       | `qiftop::backend::procsnap::pidStartTime` — `/proc/<pid>/stat` field-22 parser robustness (commands with spaces / parens / nested quotes), live self-PID round-trip, missing PID returns nullopt. |
| `attribution_docker` (Tier-2) | Live end-to-end: `runners/run-docker.sh` brings up an alpine container, drives container→host TCP flow, `qiftop-attribution-probe` asks the production resolver chain to attribute the flow back to `runtime=docker` + the right CID prefix. Gated by `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON` (default OFF). |
| `attribution_podman` (Tier-2) | Sibling of `attribution_docker` using rootful podman + netavark; exercises the `libpod-<id>.scope` cgroup hierarchy that the docker path never produces. SKIPs cleanly on hosts where rootful podman can't start a container (e.g. logind rlimit-delegation quirks). |
| `attribution_k3d` (Tier-2) | k3s-in-docker (k3d). Exercises the **nested** container chain (`docker → kubernetes → containerd`, depth 3) — the leaf-wins segment-walk has to land on the innermost containerd CID, not on the outer k3s node container. Local-only (Vagrant); not in CI (cold k3d image pull is ~3–4 min, the chain shape is already pinned by Tier-1 fixtures). |
| `attribution_k8s` (Tier-2) | "Naked" k8s via **k0s --single** (single-binary, no docker wrapper). Distinguishing assertion vs. k3d: chain depth is exactly 2 (`kubernetes → containerd`) and the JSON MUST NOT contain `"runtime":"docker"`. Catches phantom-wrapper bugs in `classifyPathChain`. Local-only (Vagrant). |
| `attribution_systemd_dbus` (Tier-2) | The ONLY runner that exercises the deployed **systemd unit + DBus policy** instead of the in-process probe. Installs the freshly-built `qiftop-agent` component (`cmake --install --component qiftop-agent --prefix /usr`), starts it under systemd, runs a docker container holding ONE long-lived external flow (`sleep 3600 \| nc <host> 22`), then asserts over DBus (`sudo busctl … GetConnections`, JSON parsed in `run-systemd-dbus.sh`) that the flow attributes to `runtime=docker` + CID prefix. Guards the sandbox surface (`RestrictNamespaces`, `CapabilityBoundingSet`) that the probe runners bypass — e.g. the `RestrictNamespaces=yes` regression in §8a rule 5. DESTRUCTIVE / VM-CI-only (needs passwordless sudo + `QIFTOP_BUILD_DIR`). Run via `scripts/local-integration.sh --runtime systemd-dbus`. See §6.5b for the container-flow-generation wisdom it encodes. |

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

`.github/workflows/integration.yml` runs the Tier-2 attribution
runners (docker + podman only) on push-to-main / dispatch / release.
k3d and k8s runners exist but only run locally via the Vagrant
harness — cold bring-up is 3–4 min each and the chain shapes are
already pinned by Tier-1 unit tests
(`test_cgroup_parse::k8sCgroupfsDriverK3dShape`,
`k8sNakedCgroupfsDriver`, `k8sNakedSystemdDriver`).

### 6.5a Vagrant runner ordering (local Tier-2)

The Vagrant VM at `tests/integration/vagrant/` is the supported home
for the heavyweight runners (k3d, naked k8s). One sharp edge to
remember when iterating:

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
  + `groupLabelFor()`; the rest is automatic.
* **Process / Container columns are capability-gated, not always-on.**
  `Column::Process` and `Column::Container` are hidden by default; the
  Settings dialog's "Process & Container Attribution" sub-section
  (Display tab) advertises three toggles — `Settings::showProcessColumn`
  / `showContainerColumn` / `showContainerChainInTooltip` — each
  enabled only when the agent advertises the matching wire token
  (`process-attribution-wire` / `container-attribution-wire` /
  `container-chain-wire`). The values persist regardless so they take
  effect when the user later runs against an attribution-capable
  agent. `applySettingsToUi()` is the single point where the
  user-pref AND the wire-token are AND-ed together to (un)hide each
  column — never `setColumnHidden()` either column outside that
  helper. The header right-click menu's Process / Container entries
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
