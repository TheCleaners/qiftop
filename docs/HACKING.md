# HACKING.md — qiftop hacker's handbook

A practical, recipe-oriented companion to [AGENTS.md](AGENTS.md). Where
AGENTS.md describes *what the system is* (architecture, contracts, layering
rules, test plan), HACKING.md tells you *how to actually work on it*.

If you are an LLM agent picking this repo up cold:

* Read AGENTS.md first for the architecture and the DBus contract.
* Read this file for build/run/debug recipes and conventions.
* When you make a major change or refactor, update both:
  * AGENTS.md gets a changelog line + any contract/layering edits.
  * HACKING.md gets any new recipe, debugging tip, or dev-loop change.

---

## 1. Prerequisites

### Debian / Ubuntu

```bash
sudo apt install --no-install-recommends \
    build-essential cmake pkg-config ninja-build \
    qt6-base-dev qt6-base-dev-tools libqt6dbus6 libqt6network6 \
    libnl-3-dev libnl-route-3-dev libnetfilter-conntrack-dev \
    dbus dpkg-dev fakeroot
```

### Arch

```bash
sudo pacman -S --needed base-devel cmake ninja \
    qt6-base \
    libnl libnetfilter_conntrack \
    dbus
```

### Fedora

```bash
sudo dnf install -y \
    gcc-c++ cmake ninja-build pkgconf-pkg-config \
    qt6-qtbase-devel qt6-qtbase-private-devel \
    libnl3-devel libnetfilter_conntrack-devel \
    dbus-daemon
```

If `cmake` complains about a missing Qt6 component, search the distro
package list — every component (`Core`, `Widgets`, `Network`, `DBus`) is
usually in `qt6-base*-dev`/`qt6-qtbase-devel`.

---

## 2. Build

### One-time configure

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Useful overrides:

| Cache var                       | Default | Meaning                                              |
|---------------------------------|---------|------------------------------------------------------|
| `CMAKE_BUILD_TYPE`              | (empty) | `Debug`, `Release`, `RelWithDebInfo`.                |
| `CMAKE_INSTALL_PREFIX`          | `/usr/local` | Where `cmake --install` lands. Use `/usr` for `.deb`. |
| `QIFTOP_BUILD_TESTS`            | `OFF`   | Build the (forthcoming) test suite under `tests/`.   |
| `QIFTOP_AUTO_PACKAGE`           | `ON`    | Run `cpack -G DEB` automatically after each agent re-link. |

### Incremental rebuild

```bash
cmake --build build -j$(nproc)
```

Both targets (`qiftop`, `qiftop-agent`) build from the same tree; CMake
deduplicates the shared backend objects automatically.

### Packaging

Both `.deb`s are **regenerated automatically on every build** that re-links
`qiftop-agent` (which is the natural moment to repackage — by then `qiftop`
has linked too). This is a developer convenience; disable with:

```bash
cmake -S . -B build -DQIFTOP_AUTO_PACKAGE=OFF
```

To force a manual run (rare):

```bash
cd build
cpack -G DEB
```

Output:
```
qiftop_<ver>_amd64.deb         (GUI + .desktop + icon)
qiftop-agent_<ver>_amd64.deb   (daemon + systemd + dbus policy + /etc conffile)
```

`dpkg-deb -c <file>.deb` to inspect contents; `dpkg-deb -e <file>.deb /tmp/d`
then `cat /tmp/d/conffiles` to confirm config files are correctly tagged.

> **Implementation note** (don't repeat the mistake): the auto-package step
> uses `add_custom_command(TARGET qiftop-agent POST_BUILD …)`, **not** an
> `add_custom_target(... ALL)` with file-level `DEPENDS`. The latter caused
> an infinite reconfigure loop because cpack writes install manifests into
> the build dir that look to the generator like "downstream targets are
> stale", triggering re-cmake → re-cpack → … See `CMakeLists.txt` near the
> bottom and the comment block there.

---

## 3. Running locally

### Without installing — session bus (most dev work)

Two terminals.

**Terminal A — agent (no root, conntrack will gripe and that's fine):**

```bash
./build/qiftop-agent --session --verbose -c dist/conf/agent.conf
```

**Terminal B — GUI:**

```bash
# Force the GUI to also use the session bus when talking to the agent:
DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS ./build/qiftop --verbose
```

(Or pass `--no-agent` to the GUI to use in-process backends without the
agent at all.)

### Without installing — system bus (the real path)

Requires root for the agent and an installed DBus policy. Easiest:
`cmake --install build --prefix /usr` then start the systemd unit:

```bash
sudo cmake --install build --prefix /usr
sudo systemctl daemon-reload
sudo systemctl start qiftop-agent.service
./build/qiftop --verbose
```

**Group access (post-hardening, June 2026).** The system-bus policy
(`dist/dbus/org.qiftop.NetworkAgent1.conf`) only permits members of the
`netdev` group to call agent methods. If your dev user isn't already a
member, the GUI silently falls back to its in-process backend (which
needs self-elevation via pkexec each launch — annoying for dev). One-time
fix:

```bash
sudo usermod -a -G netdev "$USER"
# then log out + back in (group membership is set at session start);
# or for a single shell session: `newgrp netdev`
```

The Debian `postinst` creates the `netdev` group if it doesn't already
exist, but never auto-adds users to it (we don't silently relax
permissions). On distros without a stock `netdev` group, either add it
(`sudo groupadd -r netdev`) or change the `<policy group="netdev">`
stanza to a group that exists on your system.

`agentReachable()` (in `src/main.cpp`) probes the agent with a real
`GetInterfaces` call now, so a user who isn't in `netdev` falls back
cleanly to the in-process backend instead of staring at an empty UI.

### Iftop-style filtering

```bash
./build/qiftop -i wlan0 -i eth0
```

Opens on the Connections tab, restricted to those interfaces, for the
current session only (not persisted).

---

## 4. Talking to the agent by hand

Adjust `--user` to `--system` once you've installed system-wide.

```bash
# Introspect:
busctl --user introspect org.qiftop.NetworkAgent1 /org/qiftop/NetworkAgent1/Interfaces

# Fetch snapshot:
busctl --user call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces GetInterfaces

# Request faster polling (250 ms) — hint auto-expires after idle.hint_ttl_secs:
busctl --user call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces SetDesiredIntervalMs u 250

# Subscribe to live signals:
dbus-monitor --session "interface=org.qiftop.NetworkAgent1.Interfaces"

# Probe contract version + capability tokens (clients use these to gate
# optional behaviour). Empty/missing == pre-property agent; treat as legacy.
busctl --user get-property org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces Version
busctl --user get-property org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces Capabilities
```

`qdbus6 org.qiftop.NetworkAgent1` works too if you prefer that tool.

To inspect the raw wire format of a snapshot signal (useful when
debugging a DTO mismatch between client and agent — e.g. after a
breaking-then-merged reshape that left a stale alpha .deb on the
system):

```bash
# Stream signals with type sigs so you can see the exact tuple shape
# the agent is emitting right now.
gdbus monitor --session --dest org.qiftop.NetworkAgent1 \
    --object-path /org/qiftop/NetworkAgent1/Connections

# Or, dump introspection XML to confirm signal sigs match what
# AGENTS.md §4 documents:
busctl --user introspect --xml-interface \
    org.qiftop.NetworkAgent1 /org/qiftop/NetworkAgent1/Connections \
    | grep -E '<(signal|method)'
```

Note (system-bus only): the policy in `dist/dbus/` restricts callers to
members of the `netdev` group. From a non-netdev shell, every method call
will return `AccessDenied`; this is the correct production behaviour. Run
`busctl --system …` from a netdev shell (or use `sudo -g netdev busctl …`).

---

## 5. Common dev tasks (cookbook)

### 5.1 Add a new DBus method to the agent

1. Declare the method as a `public slots:` member of
   `InterfacesService` or `ConnectionsService` in the corresponding `.h`.
   * For methods that need the caller's bus name, the class must inherit
     `protected QDBusContext` (both already do) and you call
     `calledFromDBus()` / `message().service()` inside the slot.
2. Implement it in the matching `.cpp`. Begin with
   `if (m_idle) m_idle->noteActivity();` if it should count as activity.
3. Re-build. Qt auto-MOC picks it up; the method is automatically exposed
   because we register the objects with `QDBusConnection::ExportAllContents`.
4. **Bus policy:** the system-bus policy is group-gated
   (`<policy group="netdev">`); any new method you add inherits that gate
   automatically. If you need a different access model for one method
   specifically, add a `<deny send_member="MethodName"/>` under the
   default policy and a matching `<allow>` under the privileged group.
5. Add a client-side wrapper in `src/backend/dbus/DBus*Monitor.{h,cpp}` if
   the GUI is going to use it.
6. Document the new method in AGENTS.md §4 (the DBus contract table).

### 5.2 Add a new config key

1. Add the field to `IdleManager::Config` (or wherever appropriate) with a
   sane default.
2. Read it in `loadIdleConfig()` (`src/agent/Config.cpp`) — and **always
   route the parsed value through `clampCfg()`** so a typo in
   `/etc/qiftop/agent.conf` produces a `qWarning()` instead of a degenerate
   cadence. Pick bounds that make sense (millisecond intervals: `[10,
   3'600'000]`; window/timeout: `[0, 86'400'000]`, with `0` meaning
   "disable that step").
3. Document it in `dist/conf/agent.conf` with a top-of-section comment
   block and inline `Default: …` line — that file IS the documentation.
   If the value can be `0` to disable, say so explicitly; admins read the
   conffile, not the source.
4. The file is already a Debian conffile (see `dist/debian/conffiles`),
   so additions are upgrade-safe.

### 5.3 Add a new backend (e.g. macOS, BSD)

1. Create `src/backend/<platform>/` with implementations of
   `NetworkMonitor` and `ConnectionMonitor`. They must implement
   `start()`, `stop()`, and the new `setPollIntervalMs(int)` (≤0 = pause).
2. Add a `backend_<platform>` static library in `CMakeLists.txt`, guarded by
   a `if(CMAKE_SYSTEM_NAME STREQUAL "...")` block.
3. Link both `qiftop` and `qiftop-agent` against it under the same guard;
   define a `BACKEND_<PLATFORM>` compile flag and adjust the `#ifdef`s in
   `src/agent/main.cpp` and `src/main.cpp`.
4. Do **not** include backend headers from `ui/` or `agent/*Service.{h,cpp}`
   — they only know about the abstract base classes (AGENTS.md §2).

### 5.4 Change a DBus DTO

**During v0.1 alpha pre-releases:** reshape freely. Append the new
field at the END of the struct (DBus struct sigs hash the field list
— reordering breaks subscribers), update the matching `<<` / `>>`
operators in `src/dbus/Types.cpp` in the SAME declaration order,
extend `toDto` / `fromDto`, bump `kAgentVersion` in
`InterfacesService.cpp` and add a capability token describing the new
behaviour. Also update AGENTS.md §4 (signature string + field list +
capabilities table) in the same commit — the two drift trivially if
you split them across commits, and the next contract review will
waste cycles re-discovering it.

Clamp every newly-added wire-sourced enum on the receive path in
`fromDto`: `(d.x <= quint8(Enum::Max)) ? static_cast<Enum>(d.x) :
Enum::SafeDefault`. Don't `static_cast` directly — a buggy or
future-extended sender will produce UB on the receiver otherwise.
Add a clamp test in `tests/test_dbus_types.cpp`.

**Post-v0.1-stable:** breaking change. Bump the interface name to
`NetworkAgent2`, keep `NetworkAgent1` alive for one release. See
AGENTS.md §8.

### 5.5 Adding tests

Unit tests live under `tests/` and are built by default
(`-DQIFTOP_BUILD_TESTS=ON`, on unless explicitly turned off).

* **Framework:** Qt's own `QTest` — no external deps. `find_package(Qt6
  COMPONENTS Test)` is already wired in `tests/CMakeLists.txt`.
* **One executable per test file.** Each `tests/test_foo.cpp` is its own
  binary registered with `add_test()`, so CTest reports them
  individually and a crash in one doesn't sink the rest.
* **Recipe:**
  1. Drop `tests/test_foo.cpp`. Use `QTEST_APPLESS_MAIN` for tests that
     touch no `QCoreApplication`-dependent machinery (most pure-logic
     tests); use `QTEST_MAIN` if you need an event loop or QSettings
     with the default constructor.
  2. Append `qiftop_add_test(test_foo test_foo.cpp [extra_sources...])`
     to `tests/CMakeLists.txt`. If the test needs a non-test source
     compiled in (e.g. `Settings.cpp` for the migration test), list it
     as an extra source — easier than introducing a static lib just for
     tests.
  3. `cmake --build build && (cd build && ctest --output-on-failure)`.

* **What's worth testing here:** pure helpers — the heuristics in
  `src/util/ConnectionHeuristics.h`, `Settings` migration on the
  `QSettings` ini path, `Exporter` formatting, anything in `util/`
  with no Qt-widget dependency. Don't try to spin up `MainWindow`;
  the model+delegate stack is hard to fixture and most regressions
  surface in the headless pure-logic layer anyway.

* **Hermetic QSettings:** for any test touching `Settings`, redirect
  the ini path into a `QTemporaryDir` before constructing it. See
  `tests/test_settings_migration.cpp`:
  ```cpp
  QCoreApplication::setOrganizationName("qiftop-test");
  QCoreApplication::setApplicationName("qiftop-test");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
  ```
  Also call `QStandardPaths::setTestModeEnabled(true)` in
  `initTestCase()` so any `QStandardPaths::writableLocation()` lookups
  go to a sandboxed cache dir.

* **Qt link gotcha:** anything that includes `<QHostAddress>` (e.g.
  `ConnectionHeuristics.h`) needs `Qt6::Network` even if the test
  itself never instantiates a socket — `QHostAddress` lives in
  QtNetwork. `qiftop_add_test()` already links it.

* **Integration tests** (need a live agent) — same pattern but spawn
  `qiftop-agent --session --config <tmpdir>/agent.conf` on a private
  session bus and tear down by recorded PID. None exist yet; add under
  `tests/integration/` with its own `add_subdirectory()` guard.

* **Sanitizers (`-DQIFTOP_TESTS_SANITIZE=…`)** — opt-in per build dir,
  applied **only to test targets** so production binaries stay clean.
  Accepted values: `OFF` (default), `address`, `undefined`,
  `address+undefined` (recommended for CI), `thread`, `leak`.
  ```sh
  cmake -B build-san -DCMAKE_BUILD_TYPE=Debug \
      -DQIFTOP_TESTS_SANITIZE=address+undefined
  cmake --build build-san -j$(nproc)
  ctest --test-dir build-san --output-on-failure
  ```
  Per-test env (set by `qiftop_add_test()` when sanitizers are on)
  includes `abort_on_error=1`, `halt_on_error=1`, full symbolisation
  and LSan stack traces. False-positive leaks from Qt/glib/dbus
  one-shot statics are suppressed via `tests/lsan.supp` — if you add
  a new third-party dependency that leaks at process exit, append a
  narrow `leak:libfoo.so` entry there (never broaden existing ones).
  `thread` is mutually exclusive with `address`; use a separate build
  dir for each.

### 5.6 The "pure-logic header" extraction pattern

When you find yourself wanting to unit-test a lambda or member function
that touches no Qt signals/slots/event-loop, extract it into a
header-only namespace under `src/<area>/`. Existing example:
`src/util/ConnectionHeuristics.h` carries `inferDirection`,
`isForwardedFlow`, `emaUpdate`, `emaUpdateAsym`, `easeOutCubic`. The
`ConnectionModel` then becomes a thin caller. Wins:

* Tests don't need to instantiate the model, its DNS resolver, its
  delegate, or a proxy.
* The logic is reusable from the agent side too if that ever becomes
  relevant.
* The header is allowed to depend on Qt value types (`QHostAddress`,
  `QSet`, `QString`) but **not** on `QObject` / signals / event loop /
  widgets. Keep that invariant — it's the whole point.

---

### 5.7 Per-connection rate smoothing pipeline

The Connections view's optional rate smoothing
(`Settings::rateSmoothingMs > 0`) is a two-stage pipeline. Knowing the
split helps when adding new derived columns or tweaking visuals:

1. **Target (noise rejection)** — symmetric EMA of *raw* (un-smoothed)
   per-poll rates with τ = `rateSmoothingMs`. Lives in
   `Row::rxTarget` / `txTarget`. Computed in `updateConnections()`.

2. **Display (animation tween)** — `Row::rxRate` / `txRate` are
   animated from their prior value to the target via `easeOutCubic`
   over a captured duration: `pollIntervalMs` for *rises*, `max(100ms,
   pollMs/3)` for *falls* (asymmetric duration is what makes rate
   drops feel snappier than rate spikes). The tween is advanced by
   `ConnectionModel::advanceSmoothing()`, driven by a
   `MainWindow::m_smoothingTick` QTimer at
   `max(100ms, pollIntervalMs/4)` — i.e. always at least 4× the data
   poll cadence, with a 10 fps floor.

3. **Reference (throughput gauge denominator + Max columns)** —
   computed from *raw* samples (`Row::rxSamples` deque + `rxSum/samples`
   counters). This is deliberately decoupled from the smoothed
   display: smoothing affects how things look, not how big the
   "max-so-far" reference is.

If you add a new derived/display column, decide where it sits:
* "How fast is this flow right now?" → `rxRate` (display, animated).
* "What does the gauge fill against?" → `rxReference(r)` (raw-based).
* "What did the user actually transfer?" → `rxBytes` (counter).

`advanceSmoothing()` emits `dataChanged` only for rows whose displayed
value moved by more than 1 B/s; a finished tween pins the value to its
target and stops emitting. Don't add per-tick work that runs on every
row unconditionally — the sub-poll tick fires often and is on the GUI
thread.

---

## 6. Debugging recipes

### Agent won't acquire the system bus name

```bash
journalctl -u qiftop-agent.service -e --no-pager
sudo cat /usr/share/dbus-1/system.d/org.qiftop.NetworkAgent1.conf
```

The policy file must allow `<allow own="org.qiftop.NetworkAgent1"/>` for the
root user. If you renamed the service, the policy needs updating too.

### GUI silently falls back to in-process backend / "Relaunch as administrator"

Most common cause after the June 2026 hardening: your user isn't in the
`netdev` group, so the DBus policy denies every method call. The
`agentReachable()` probe in `src/main.cpp` calls `GetInterfaces` with a
1 s timeout; AccessDenied makes it return `false` and the GUI drops into
the in-process path. To confirm:

```bash
busctl --system call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Interfaces \
    org.qiftop.NetworkAgent1.Interfaces GetInterfaces
# "Access denied" → you're not in netdev.

groups | tr ' ' '\n' | grep -x netdev || echo "not in netdev"
sudo usermod -a -G netdev "$USER"   # then log out + back in
```

### Polling looks frozen

Run the agent with `--verbose`. Look for the IdleManager log line:

```
qiftop.verbose: IdleManager: polling interval -> paused
```

That's expected after `idle.timeout_secs` with no method calls. Next call
unpauses it. If a client wants sub-second updates, it must call
`SetDesiredIntervalMs(<ms>)` at most every `idle.hint_ttl_secs / 2` seconds
to keep the hint alive.

If you set `idle.timeout_secs=0` in `/etc/qiftop/agent.conf` and the
agent is still pausing, you're on a pre-hardening build — that value
used to invert and pause immediately. Current behaviour: `0` for any
schedule window or timeout means "disable that step", as documented.

### Conntrack EPERM

```
ConntrackMonitor: nfct_query(NFCT_Q_DUMP) failed: Operation not permitted
```

Only expected when the agent runs as a non-root user (e.g. during dev with
`--session`). The systemd unit grants `CAP_NET_ADMIN`; in production this
should not appear.

### Stale .deb after `cpack`

The auto-package `add_custom_command(POST_BUILD)` is wired to the
`qiftop-agent` target only — client-only edits (e.g. anything in
`src/ui/`) build & link the new `qiftop` binary but **do not** trigger
a `.deb` regen, so `dpkg -i build/qiftop_0.1_amd64.deb` keeps
installing the *previous* client. Symptom: edits seem to have no
effect at runtime even though the build succeeded.

Two fixes:

```bash
# Force a repackage by hand:
(cd build && cpack -G DEB && sudo dpkg -i qiftop_0.1_amd64.deb)

# Or touch the agent so the POST_BUILD hook fires:
touch src/agent/main.cpp && cmake --build build -j$(nproc)
```

If the cpack manifest itself looks stale, nuke and retry:

```bash
rm -rf build/_CPack_Packages build/*.deb && (cd build && cpack -G DEB)
```

### Ambiguous shortcut overload at runtime

```
QAction::event: Ambiguous shortcut overload: Ctrl+Q
```

means the same key sequence is bound twice in routing scope. Common
causes in this tree:

* Setting `setShortcuts({QKeySequence::Quit, "Ctrl+Q"})` on the same
  action — on Linux/Windows `QKeySequence::Quit` *is* `Ctrl+Q`, so the
  list registers it twice. Use one or the other, not both.
* Combining `action->setShortcutContext(Qt::ApplicationShortcut)` with
  *both* `mainWindow->addAction(action)` and a standalone
  `QShortcut(...)` for the same sequence. Pick one delivery path.

The action is parented to a `QMenuBar` that the user can hide; that's
why we need `Qt::ApplicationShortcut` in the first place (shortcuts on
actions of a hidden widget tree fall out of Qt's routing). Just don't
double-register.

### Stuck old agent process during dev

```bash
pgrep -a qiftop-agent
kill <PID>   # by literal PID; never `pkill`/`killall` in this tree (style)
```

### QToolBar widget stretching unexpectedly

When you wrap multiple widgets in a `QWidget` container and `addWidget()`
it to a `QToolBar`, the toolbar treats the container as a single
toolbar-action widget with its default `QSizePolicy::Preferred`
horizontal policy — it can grow. If the container has a child with a
**maximum width cap** (e.g. a `QLineEdit::setMaximumWidth(440)`), the
toolbar's extra space cannot land in the capped widget and instead
gets eaten by the layout's spacing between siblings — you get a big
gap between, say, a label and its line edit.

Two complementary fixes:

1. Set the container's horizontal policy to
   `QSizePolicy::Maximum` so the toolbar can't stretch it past the
   children's combined natural width.
2. To intentionally push a widget group to the right edge of the
   toolbar, insert an explicit expanding spacer **before** the group:

   ```cpp
   auto *spacer = new QWidget(toolbar);
   spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
   toolbar->addWidget(spacer);
   ```

   It has zero minimum width, so when the window is too narrow the
   spacer collapses first and the group falls back to sitting next to
   its left neighbour rather than being clipped.

### Persistent help popup (vs. fleeting `QToolTip`)

`QToolTip::showText()` has its own dismiss timer and is also killed by
focus changes, mouse leaves, and various other Qt-internal events.
It's wrong for "click ? to read syntax help" — users invariably mouse
over the tooltip to read it, and it vanishes.

For a click-to-summon help popup that stays open until dismissed, use a
`QFrame` with the `Qt::Popup` window flag — it auto-closes on outside
click or Escape but otherwise persists, and you can put any
rich-text-aware widget inside. See `MainWindow::showFilterHelp()` for
the pattern (built lazily and reused).

---

## 7. Style & conventions (quick reference)

(Full reasoning lives in AGENTS.md §7.)

* Qt 6, C++20. 4-space indent, no tabs. Brace placement: Allman for
  function/class bodies, K&R for everything else; this is how the existing
  code is written.
* Headers list includes in three groups separated by blank lines:
  1. Project-local headers (matching `#include "Foo.h"` for the .cpp's
     primary subject first).
  2. Qt headers, alphabetised.
  3. System / standard library headers.
* `QStringLiteral("literal")` only with literal tokens; for
  `constexpr auto kFoo = "..."` use `QString::fromLatin1(kFoo)`.
* Logging:
  * Diagnostic, opt-in: `qCInfo(lcVerbose) << ...` (controlled by
    `--verbose`).
  * Anomalies that the user should know about: `qWarning()`.
  * Hard errors that abort startup: `qCritical()`.
* DBus services that expose `public slots:` methods must be registered with
  `QDBusConnection::ExportAllContents`. Properties additionally need
  `Q_PROPERTY` + a `NOTIFY` signal.
* `Settings` setters that persist must `store()` AND `emit changed()`.
  Transient setters (e.g. `setConnectionVisibleIfacesTransient`) must
  only `emit changed()` — do not call `store()`.
* Comments explain *why*, not *what*. Trivial code stays uncommented.
* **Never** call `.begin()` / `.end()` on two separate expressions that
  each return a container by value — the iterators point into different
  temporaries and `std::distance` between them is UB. Always bind the
  return value to a named local first:
  ```cpp
  // WRONG: two distinct temporaries — UB, has caused a SIGABRT in this tree.
  QSet<QString> s(obj->list().begin(), obj->list().end());

  // RIGHT:
  const QStringList list = obj->list();
  QSet<QString> s(list.begin(), list.end());
  ```

---

## 8. Pull-request checklist

Before opening / merging:

- [ ] `cmake --build build -j$(nproc)` succeeds clean (no warnings beyond
      existing ones).
- [ ] If you touched anything under `dist/` or `CMakeLists.txt`'s install
      rules, rebuild and inspect both `.deb`s with `dpkg-deb -c`.
- [ ] If you added a DBus method, removed one, or changed a signature:
      * AGENTS.md §4 table updated.
      * `NetworkAgent` interface name bumped if it was breaking.
- [ ] If you added a config key:
      * `dist/conf/agent.conf` has a top-comment block for it.
      * `loadIdleConfig()` (or successor) reads it with a sensible default.
- [ ] If you made a major change or refactor:
      * AGENTS.md changelog line appended.
      * HACKING.md updated where any recipe/debug tip is now wrong or new.
- [ ] No `pkill`/`killall` in scripts — always literal `kill <PID>`.
- [ ] No `cat`/`head`/`tail`/`find`/`ls` in CI scripts when a Qt/CMake
      facility already exists.

---

## 9. Release

### Automated (preferred)

CI is wired up in `.github/workflows/`:

* **`ci.yml`** — runs on every push / PR to `master`/`main`. Native
  ubuntu-24.04 runner × Debug + Release, plus a containerised matrix
  for `ubuntu:26.04` and `fedora:44` (× Debug + Release). The
  containerised slot for 26.04 is a temporary measure until GitHub
  Actions publishes a native `ubuntu-26.04` runner image — swap it
  back to a `runs-on:` entry as soon as that's available. Builds with
  Ninja, runs `ctest` under `dbus-run-session` with
  `QT_QPA_PLATFORM=offscreen`.
* **`release.yml`** — triggered on `v*` tag push. Builds .debs on a
  clean ubuntu-24.04 runner, verifies the tag matches
  `project(qiftop VERSION ...)`, computes SHA256SUMS, and publishes a
  GitHub Release with both .debs + checksums attached. Release notes
  are auto-generated by GitHub (diff vs. the previous tag), with
  category grouping configured in `.github/release.yml`. Tags
  containing `-` (e.g. `v0.2-rc1`) are marked as prerelease.

To cut a release:

```bash
# 1. Bump project version (single source of truth — both .debs and
#    CPACK_PACKAGE_VERSION derive from it).
vim CMakeLists.txt                       # project(qiftop VERSION X.Y ...)
vim AGENTS.md                            # append a changelog entry
vim HACKING.md                           # bump Last-reviewed line

# 2. Commit + tag + push.
git commit -am "release: X.Y"
git tag -a vX.Y -m "qiftop X.Y"
git push origin master vX.Y
```

The `release.yml` workflow will refuse to publish if `CMakeLists.txt`'s
`project()` VERSION doesn't match the tag base (`vX.Y-rc1` → `X.Y`).
That's deliberate — keeps the .deb version, the GitHub release tag,
and `qiftop --version` consistent.

### Manual fallback

If GitHub Actions is unavailable, the same steps locally:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
(cd build && cpack -G DEB)
```

Smoke-install on a clean container:

```bash
sudo apt install ./build/qiftop-agent_X.Y_amd64.deb \
                  ./build/qiftop_X.Y_amd64.deb
sudo systemctl status qiftop-agent
qiftop --verbose
```

Then upload the `.deb`s + a `sha256sum *.deb > SHA256SUMS` manually
via the GitHub Releases UI.

---

_Last reviewed: 2026-06-07._
