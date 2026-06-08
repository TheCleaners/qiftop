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

| Object path                                  | Interface                                  | Methods                                                   | Signals                                                | Properties                  |
|----------------------------------------------|--------------------------------------------|-----------------------------------------------------------|--------------------------------------------------------|-----------------------------|
| `/org/qiftop/NetworkAgent1/Interfaces`       | `org.qiftop.NetworkAgent1.Interfaces`      | `GetInterfaces()`, `SetDesiredIntervalMs(u)`              | `StatsChanged`, `CadenceChanged(u)`                    | `Version: s`, `Capabilities: as` |
| `/org/qiftop/NetworkAgent1/Connections`      | `org.qiftop.NetworkAgent1.Connections`     | `GetConnections()`, `SetDesiredIntervalMs(u)`             | `ConnectionsChanged`, `PermissionDenied`, `accountingChanged` |                             |

DTOs live in `src/dbus/Types.h`. The connection signature is
`a(yysqysqtttts)` (family, l3proto, src, sport, dst, dport, l4proto, state,
bytes_out, pkts_out, bytes_in, pkts_in, iface).

### Contract version & capabilities

`Version` is a free-form string (currently `"0.1"`) bumped only for
*additive* changes to the agent surface that clients may care about.
**Breaking** changes (DTO signature, method removal/rename) still require
a fresh `org.qiftop.NetworkAgent2` interface per §8.

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

Add a token here when shipping a new optional behaviour; **never remove
or rename a token** — pre-existing clients use them as feature flags.

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

### 6.3 Gaps worth filling

1. **`dbus::Types` round-trip** — `Connection` ↔ `ConnectionDto` ↔
   `QDBusArgument`. Pure marshalling; should be trivial to test.
2. **`ConntrackMonitor::Worker` per-flow diff math** — once extracted
   per §6.4 #3, the diff/accounting logic can be exercised without a
   live conntrack handle.
3. **End-to-end with a real conntrack table** — requires root; needs a
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
  pass through hostile env vars either.
* **CSV / spreadsheet injection.** `src/util/Exporter.cpp::csvSanitise`
  prepends a leading apostrophe to any field starting with `=`, `+`, `-`,
  `@`, `\t`, or `\r` before quoting. Attacker-controlled hostnames (via
  reverse DNS) or interface names from the kernel must not be able to
  execute as spreadsheet formulas when the user opens an exported CSV.

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
  test), update [HACKING.md](HACKING.md) too.
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
