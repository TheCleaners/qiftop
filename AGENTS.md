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
| `/org/qiftop/NetworkAgent1/Connections`      | `org.qiftop.NetworkAgent1.Connections`     | `GetConnections()`, `SetDesiredIntervalMs(u)`             | `ConnectionsChanged(t, a(...))`, `PermissionDenied`, `AccountingChanged` | |

Both data signals carry a leading `quint64 monotonicMs` (a
`QElapsedTimer`-based, agent-process-local monotonic millisecond
counter) sampled at the moment the snapshot was serialised. This is
the canonical timestamp for downstream rate computation (libqiftop
history, Prometheus exporter, alerts): it's immune to wall-clock
jumps and to DBus delivery jitter. It is NOT comparable across agent
restarts.

DTOs live in `src/dbus/Types.h`.

**Connection wire signature** (15 fields per flow):
`a(yysqysqttttsyuy)` =
`(proto, localFamily, localAddress, localPort, remoteFamily, remoteAddress,
remotePort, rxBytes, txBytes, rxPackets, txPackets, iface, direction,
ifIndex, tcpState)`.

**InterfaceStats wire signature** (16 fields per row):
`(ssusasttttbbuytttt)` =
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

### Contract version & capabilities

`Version` is a free-form string (currently `"0.3"`) bumped only for
*additive* changes to the agent surface that clients may care about.
**Breaking** changes (DTO signature, method removal/rename) still require
a fresh `org.qiftop.NetworkAgent2` interface per §8.

> Historical note: the 0.1 → 0.2 bump (pre-release v0.1-alpha2 →
> alpha3) reshaped the wire (added `direction` byte, switched `proto`
> to IANA, PascalCased `AccountingChanged`). The 0.2 → 0.3 bump
> (alpha3 → tag tbd) extended both DTOs and added a `monotonicMs`
> leading arg to both data signals. Since only pre-release alphas
> existed in either window we reshaped in place rather than branching
> `NetworkAgent2`. Older alpha clients failing to unmarshal fall back
> cleanly to the in-process backend via the existing probe. Post-v0.1
> stable release, breaking changes MUST go through `NetworkAgent2`.

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
| `test_filter`              | Filter mini-language parser + evaluator (every field/op)          |
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
| `test_dbus_types`          | `ConnectionDto` wire round-trip: IANA proto mapping, direction field, out-of-range direction clamp |

### 6.3 Gaps worth filling

1. **`ConntrackMonitor::Worker` per-flow diff math** — once extracted
   per §6.4 #3, the diff/accounting logic can be exercised without a
   live conntrack handle.
2. **End-to-end with a real conntrack table** — requires root; needs a
   CI runner with `CAP_NET_ADMIN` or a privileged container.

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
ubuntu-22.04 + ubuntu-24.04 × Debug + Release. `HOME` is redirected to
`$RUNNER_TEMP/home` so QSettings/Autostart tests don't trample the
runner user. See docs/HACKING.md §5.5 for the test-writing conventions.

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
