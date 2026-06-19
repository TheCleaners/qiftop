# HACKING.md — qiftop hacker's handbook

A practical, recipe-oriented companion to [AGENTS.md](AGENTS.md). Where
AGENTS.md describes *what the system is* (architecture, contracts, layering
rules, test plan), HACKING.md tells you *how to actually work on it*.

If you are an LLM agent picking this repo up cold:

* Read AGENTS.md first for the architecture and the DBus contract.
* Read this file for build/run/debug recipes and conventions.
* When you make a major change or refactor, update both:
  * AGENTS.md gets any contract/layering/architecture edits.
  * HACKING.md gets any new recipe, debugging tip, or dev-loop change.

---

## 1. Prerequisites

### Debian / Ubuntu

```bash
sudo apt install --no-install-recommends \
    build-essential cmake pkg-config ninja-build \
    qt6-base-dev qt6-base-dev-tools libqt6dbus6 libqt6network6 \
    libgl1-mesa-dev libncurses-dev \
    libnl-3-dev libnl-route-3-dev libnetfilter-conntrack-dev \
    dbus dbus-x11 dpkg-dev fakeroot
```

### Arch

```bash
sudo pacman -S --needed base-devel cmake ninja \
    qt6-base \
    libnl libnetfilter_conntrack ncurses \
    dbus
```

### Fedora

```bash
sudo dnf install -y \
    gcc-c++ cmake ninja-build pkgconf-pkg-config \
    qt6-qtbase-devel qt6-qtbase-private-devel \
    mesa-libGL-devel ncurses-devel \
    libnl3-devel libnfnetlink-devel libnetfilter_conntrack-devel \
    dbus-daemon dbus-tools
```

If `cmake` complains about a missing Qt6 component, search the distro
package list — every component (`Core`, `Widgets`, `Network`, `DBus`) is
usually in `qt6-base*-dev`/`qt6-qtbase-devel`.

Packaging / repository extras:

```bash
# Debian / Ubuntu
sudo apt install --no-install-recommends apt-utils createrepo-c gnupg rpm lintian desktop-file-utils

# Fedora
sudo dnf install -y rpm-build createrepo_c gnupg2 rpmlint desktop-file-utils
```

### NetBSD / BSD (experimental)

The BSD family builds the client side (`libqiftop`, `qiftop` GUI, `nqiftop`
TUI, `check_qiftop`) with an in-process getifaddrs + libpcap backend; the
`qiftop-agent` is Linux-only. Everything needed is in base + one pkgsrc
package:

```sh
# pkgsrc (qt6) + base curses, base libpcap, base getifaddrs/sysctl
pkgin install qt6-qtbase cmake ninja-build pkg-config git
# non-login shells need the pkgsrc/base tools on PATH:
export PATH=/usr/pkg/bin:/usr/pkg/sbin:/usr/sbin:/sbin:$PATH
```

Do NOT install pkgsrc `ncurses` — base curses is wide-capable and is what the
build uses. Capturing flows needs root (`/dev/bpf`); run `nqiftop`/`qiftop`
with `sudo` (or `doas`). See **docs/PORTABILITY.md §7** for the full BSD
implementation guide and the gotchas (wide-char curses, the sysctl
socket→PID join, DLT handling, the sysctl size-probe trap, etc.).

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
| `QIFTOP_BUILD_TESTS`            | `ON`    | Build the QtTest suite under `tests/`. |
| `QIFTOP_BUILD_INTEGRATION_TESTS` | `ON`   | Include the session-bus `test_agent_integration` test. Only matters when tests are enabled. |
| `QIFTOP_BUILD_TUI`              | `ON`    | Build `nqiftop` when ncursesw is found (`libncurses-dev` / `ncurses-devel`). |
| `QIFTOP_BUILD_DEMO`             | `OFF`   | Build the synthetic GUI/TUI demo harnesses under `docs/demo/`. |
| `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION` | `OFF` | Build Tier-2 runtime attribution tests. Requires root + container runtimes; leave off for normal dev. |
| `QIFTOP_BUILD_BENCHMARKS`       | `OFF`   | Build the opt-in `bench/` QBENCHMARK harness (see §5.8). Build these in `Release` — unoptimized numbers are meaningless. |
| `QIFTOP_BUILD_BPF_EVAL`         | `OFF`   | Build the conntrack-vs-BPF/pcap capture **measurement** harness (`bench/integration/bpf-eval/`). Linux + VM-only + root to run; never packaged, zero runtime deps. Skip-safe: missing prerequisites are skipped, not fatal. See that dir's `README.md` and the harness `DESIGN.md`. |
| `QIFTOP_ENABLE_BPF`             | `ON`    | Build the **eBPF socket-birth attribution** path (the birth+conntrack hybrid). Needs `clang` (bpf target) + `bpftool` + `libbpf>=1.0` at build; **libbpf** only at runtime (a Recommends). Skip-safe: if the toolchain/BTF is missing the build resolves `QIFTOP_HAVE_BPF=OFF` with a STATUS line and the agent runs **conntrack-only**. Never fails configure. |
| `QIFTOP_CLANG_TIDY`             | `OFF`   | Run clang-tidy inline during compilation. Handy for local poking; the standalone script below is the cleaner report path. |
| `QIFTOP_ENABLE_LTO`            | `OFF`   | Enable link-time optimization (IPO/LTO) where the toolchain supports it. Longer link times; for release/benchmark performance. Warns and builds without it on an unsupported toolchain. |
| `QIFTOP_AUTO_PACKAGE`           | `ON`    | Run `cpack -G DEB` automatically after each agent re-link. |

> **Editor / language-server setup:** the build always emits
> `<build-dir>/compile_commands.json` (`CMAKE_EXPORT_COMPILE_COMMANDS` is on
> by default). Point clangd / VS Code at it, e.g.
> `ln -s build/compile_commands.json .` at the repo root. This same file is
> what `clang-tidy` and other compilation-database tools consume.

### Static analysis (clang-tidy)

clang-tidy is rolling out progressively, not as a surprise boss fight. The
**baseline set is now enforced (gating in CI)**: the high-signal checks from
`bugprone-*`, `clang-analyzer-*`, `performance-*`, `portability-*` and
`misc-*` (with the obvious Qt/noise traps disabled) were cleaned to zero, then
the gate flipped on, so regressions in that set now fail the `clang-tidy`
workflow. It started life as a report-only Phase 0 — that phase is done.
`bugprone-narrowing-conversions` and the `modernize-*` / `readability-*` style
families are deliberately still **off**; they're the next phases, not part of
the enforced set yet.

Local check path (same thing CI runs):

```bash
cmake -S . -B build -G Ninja -DQIFTOP_AUTO_PACKAGE=OFF
scripts/run-clang-tidy.sh build         # report: prints findings, always exits 0
scripts/run-clang-tidy.sh --gate build  # gate: exits non-zero on any finding
```

The script consumes `<build-dir>/compile_commands.json`, filters to qiftop's
own `src/` and `bench/` translation units, and skips generated moc/autogen
files. Plain `run-clang-tidy.sh build` stays report-only for quick local
poking; `--gate` is what CI uses to block regressions. On a completely fresh
build tree, run the normal build once (or at least the relevant `*_autogen`
targets) if clang-tidy complains about a missing included `.moc` file; Qt's
generator has to leave those breadcrumbs before static analysis can follow
them.

For inline diagnostics while compiling, configure with:

```bash
cmake -S . -B build-tidy -G Ninja \
    -DQIFTOP_CLANG_TIDY=ON \
    -DQIFTOP_AUTO_PACKAGE=OFF
cmake --build build-tidy --target qiftoplib
```

That path wires clang-tidy in as a compiler launcher, so it may also see Qt
generated translation units. Useful, just less tidy than the script.

The normal "build everything useful for development" configure is:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DQIFTOP_BUILD_TESTS=ON \
    -DQIFTOP_BUILD_TUI=ON \
    -DQIFTOP_BUILD_DEMO=OFF \
    -DQIFTOP_BUILD_ATTRIBUTION_INTEGRATION=OFF \
    -DQIFTOP_AUTO_PACKAGE=OFF
```

Flip `QIFTOP_BUILD_DEMO=ON` when generating screenshots/demo captures.
Flip `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON` only through the integration
drivers that provide docker/podman/k3d/k0s and the needed privileges. Leave
`QIFTOP_AUTO_PACKAGE=OFF` for fast edit/build/test loops; turn it on (or run
`cpack` manually) when you actually need fresh `.deb`s.

### Incremental rebuild

```bash
cmake --build build -j$(nproc)
```

Core targets built from the same tree:

* `qiftoplib` → `libqiftop.so` (the Widgets-free data library).
* `qiftop` → Qt Widgets GUI.
* `qiftop-agent` → privileged DBus agent.
* `nqiftop` → ncurses TUI, when `QIFTOP_BUILD_TUI=ON` and ncursesw is found.
* tests, when `QIFTOP_BUILD_TESTS=ON`.

To force one target:

```bash
cmake --build build --target qiftoplib
cmake --build build --target nqiftop
```

### ncurses TUI (`nqiftop`)

`nqiftop` lives under `src/tui/` and links only libqiftop + ncursesw. Install
`libncurses-dev` (Debian/Ubuntu) or `ncurses-devel` (Fedora), then:

```bash
cmake -S . -B build -G Ninja -DQIFTOP_BUILD_TUI=ON
cmake --build build --target nqiftop
./build/nqiftop --help
```

For development against a session-bus agent:

```bash
./build/qiftop-agent --session --verbose -c dist/conf/agent.conf
./build/nqiftop --session --verbose
```

Like the GUI, `nqiftop` probes the agent first and falls back to the
in-process Linux backend only when the agent is unavailable (that fallback
needs root for conntrack).

### libqiftop

`qiftoplib` builds the shared library whose installed name is `libqiftop.so`
with SONAME `libqiftop.so.0`. It is built by default and consumed by the GUI,
TUI, agent, and standalone examples.

Install just the runtime + development components to a local prefix:

```bash
P="$PWD/_install"
cmake --install build --component libqiftop     --prefix "$P"
cmake --install build --component libqiftop-dev --prefix "$P"
```

Downstream CMake consumers use:

```cmake
find_package(qiftop REQUIRED)
target_link_libraries(my_tool PRIVATE qiftop::qiftop)
```

Then configure the consumer with `-DCMAKE_PREFIX_PATH="$P"` (or install the
`libqiftop-dev` / `qiftop-devel` package under `/usr`). Non-CMake consumers can
use `pkg-config --cflags --libs qiftop`.

**DBus type gotcha:** every libqiftop process that talks to the agent must call
`qiftop::dbus::registerTypes()` once at startup, before creating DBus monitors
or issuing DBus calls. Without it, QtDBus cannot unmarshal replies/signals and
you see errors like:

```
type QList<qiftop::dbus::ConnectionDto> is not registered with QtDBus
```

### Building the examples against an installed libqiftop

Each `examples/*` directory is a standalone `find_package(qiftop)` project:

| Directory | Binary |
|-----------|--------|
| `examples/ndjson-stream` | `qiftop-ndjson` |
| `examples/ndjson-connections` | `qiftop-ndjson-connections` |
| `examples/prometheus-exporter` | `qiftop-exporter` |
| `examples/snapshot-export` | `qiftop-snapshot-export` |
| `examples/top-talkers` | `qiftop-top` |

CI's packaging-QA job deliberately builds them against a component-only install
(not the build tree) so the public CMake package cannot bit-rot:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQIFTOP_BUILD_TESTS=OFF \
    -DQIFTOP_AUTO_PACKAGE=OFF
cmake --build build --parallel

P="$PWD/_install"
cmake --install build --component libqiftop     --prefix "$P"
cmake --install build --component libqiftop-dev --prefix "$P"

for ex in examples/*/; do
    [ -f "$ex/CMakeLists.txt" ] || continue
    cmake -S "$ex" -B "$ex/build" -G Ninja -DCMAKE_PREFIX_PATH="$P"
    cmake --build "$ex/build" --parallel
done
```

### Packaging

The `.deb`s are **regenerated automatically on every build** that re-links
`qiftop-agent`. This is a developer convenience; disable with:

```bash
cmake -S . -B build -DQIFTOP_AUTO_PACKAGE=OFF
```

To force a manual run (rare):

```bash
(cd build && cpack -G DEB)
```

DEB output (component packages):
```
qiftop_<ver>_amd64.deb          (GUI + .desktop + icon)
qiftop-agent_<ver>_amd64.deb    (daemon + systemd + dbus policy + /etc conffile)
libqiftop0_<ver>_amd64.deb      (runtime shared library)
libqiftop-dev_<ver>_amd64.deb   (headers + CMake/pkg-config files)
nqiftop_<ver>_amd64.deb         (ncurses frontend, if built)
```

`dpkg-deb -c <file>.deb` inspects contents. To inspect control metadata without
using system temp dirs:

```bash
rm -rf _debctl && mkdir _debctl
dpkg-deb -e build/qiftop-agent_<ver>_amd64.deb _debctl
cat _debctl/conffiles
rm -rf _debctl
```

Library dependencies are auto-discovered by `dpkg-shlibdeps` (the DEB analog of
RPM's find-requires). `qiftop` and `nqiftop` only **weakly recommend**
`qiftop-agent`, so a client can install without the daemon and fall back to
in-process capture.

### Building the `.rpm`s (Fedora)

RPMs need `rpmbuild` + the Fedora `-devel` libraries. The release workflow
builds them inside `fedora:44`; locally, mirror that:

```bash
docker run --rm -v "$PWD:/src" -w /src fedora:44 bash -euxc '
  dnf -y --setopt=install_weak_deps=False install \
    cmake ninja-build gcc-c++ pkgconf-pkg-config rpm-build \
    qt6-qtbase-devel qt6-qtbase-private-devel mesa-libGL-devel \
    libnl3-devel libnfnetlink-devel libnetfilter_conntrack-devel \
    ncurses-devel dbus-daemon systemd shadow-utils
  cmake -S . -B build-rpm -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQIFTOP_BUILD_TESTS=OFF \
    -DQIFTOP_AUTO_PACKAGE=OFF
  cmake --build build-rpm --target qiftop qiftop-agent nqiftop --parallel
  cd build-rpm && cpack -G RPM -V
'
```

Docker writes `build-rpm/` as root on many hosts; run
`sudo chown -R "$(id -u):$(id -g)" build-rpm` afterwards if you need to clean
or re-use that directory.

Output (RPM naming convention, `name-version-release.dist.arch`):
```
qiftop-<ver>-1.fc44.x86_64.rpm          (GUI)
qiftop-agent-<ver>-1.fc44.x86_64.rpm    (daemon + systemd + dbus + /etc config)
qiftop-libs-<ver>-1.fc44.x86_64.rpm     (runtime shared library)
qiftop-devel-<ver>-1.fc44.x86_64.rpm    (headers + CMake/pkg-config files)
nqiftop-<ver>-1.fc44.x86_64.rpm         (ncurses frontend)
```

`rpm -qpR <file>.rpm` lists Requires (library deps auto-discovered by
find-requires); `rpm -qp --recommends <file>.rpm` shows the weak deps
(`nftables`, the GUI's `qiftop-agent` pin); `rpm -qc qiftop-agent`
confirms `/etc/qiftop/agent.conf` is `%config(noreplace)`. The CPack RPM
config lives in `CMakeLists.txt` (the `CPACK_RPM_*` block); the native
`%post`/`%postun` scriptlets are `dist/rpm/{post,postun}-agent.sh`. The
release workflow builds both `.deb` and `.rpm` on tag push.

### Building the signed apt + dnf repositories

Release assets are indexed into a static GitHub Pages tree by:

```bash
PAGES_BASE_URL="https://thecleaners.github.io/qiftop" \
GPG_KEY_ID="<project-key-id-or-empty-for-unsigned>" \
dist/repo/build-pages.sh pkgs public
```

`pkgs/` must contain the `.deb` and `.rpm` files; `public/` receives:

* `deb/` — apt repo (`Packages`, `Release`, plus `InRelease`/`Release.gpg`
  when `GPG_KEY_ID` is set).
* `rpm/` — dnf repo (`repodata/`, `qiftop.repo`).
* `qiftop-archive-keyring.{asc,gpg}` and a small landing page.

Signing model:

* apt signs repository metadata (standard apt trust model).
* dnf signs repository metadata **and** each package (`rpm --addsign`), so
  generated `.repo` files can enable both `repo_gpgcheck=1` and `gpgcheck=1`.

Headless GPG gotcha: `rpm --addsign` must be given an explicit
`__gpg_sign_cmd` that includes `--batch --pinentry-mode loopback`; otherwise
rpm's default GPG invocation can fail with `gpg exec failed` even for the
passphraseless CI key. `dist/repo/build-pages.sh` carries the known-good
definition; copy that shape if you need to sign RPMs elsewhere.

> **usrmerge gotcha**: the systemd unit installs to `/lib/systemd/system`
> (a `/usr/lib` symlink on Fedora). Without
> `CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION` listing `/lib` and the
> other shared system dirs, rpm tries to own `/lib` and conflicts with
> the `filesystem` package. The exclusion drops *directory ownership*
> only — the files inside are still packaged.

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

**Or Terminal B — TUI:**

```bash
./build/nqiftop --session --verbose
```

(Or pass `--no-agent` to the GUI/TUI to use in-process backends without the
agent at all; on Linux that path needs privileges for conntrack.)

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
exist AND auto-adds the installing user — preferring `$SUDO_USER`
(populated when `apt`/`dpkg` runs under `sudo`), falling back to
`$PKEXEC_UID` (populated when GNOME Software / KDE Discover install via
`pkexec`). The user still needs to log out and back in (or
`newgrp netdev`) for the new group membership to take effect. On
distros without a stock `netdev` group, `postinst` creates one;
the bus policy file (`dist/dbus/org.qiftop.NetworkAgent1.conf`) hard-
codes the name, so changing it requires editing that stanza too.

`probeAgent()` (in `src/main.cpp`) probes the agent with a real
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

# Connections.GetConnections returns the service's cached m_last snapshot.
# Warm the Connections service, wait for a poll tick, then read it; otherwise
# you can get a stale snapshot from before your test flow existed.
busctl --user call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Connections \
    org.qiftop.NetworkAgent1.Connections SetDesiredIntervalMs u 250
sleep 1
busctl --user call org.qiftop.NetworkAgent1 \
    /org/qiftop/NetworkAgent1/Connections \
    org.qiftop.NetworkAgent1.Connections GetConnections

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
2. Add a `backend_<platform>` static library (normally in
   `src/backend/<platform>/CMakeLists.txt`) and include it from the guarded
   platform block in the root `CMakeLists.txt`.
3. Link it into `qiftoplib` under the same guard, expose a
   `BACKEND_<PLATFORM>` compile definition, and adjust the `#ifdef`s in the
   GUI/TUI/agent entry points that instantiate concrete in-process monitors.
4. Do **not** include backend headers from `ui/` or `agent/*Service.{h,cpp}`
   — they only know about the abstract base classes (AGENTS.md §2).

### 5.4 Change a DBus DTO

Append the new field at the END of the struct (DBus struct sigs hash
the field list — reordering breaks subscribers), update the matching
`<<` / `>>` operators in `src/dbus/Types.cpp` in the SAME declaration
order, extend `toDto` / `fromDto`, bump `kAgentVersion` in
`InterfacesService.cpp` and add a capability token describing the new
behaviour. Also update AGENTS.md §4 (signature string + field list +
capabilities table) in the same commit — the two drift trivially if
you split them across commits, and the next contract review will waste
cycles re-discovering it.

Clamp every newly-added wire-sourced enum on the receive path in
`fromDto`: `(d.x <= quint8(Enum::Max)) ? static_cast<Enum>(d.x) :
Enum::SafeDefault`. Don't `static_cast` directly — a buggy or
future-extended sender will produce UB on the receiver otherwise.
Add a clamp test in `tests/test_dbus_types.cpp`.

Any breaking change after the stable DBus contract must bump the interface
name to `NetworkAgent2` and keep `NetworkAgent1` alive for one release.
See AGENTS.md §8.

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
  3. `cmake --build build && ctest --test-dir build --output-on-failure`.

* **TDD it whenever practical.** For the cgroup classifier work
  (steps 3 + Tier-1 fixtures + nspawn), the rhythm was: write the
  fixture/test FIRST → confirm it fails for the *expected reason* →
  add the regex → confirm green. This protects against the failure
  mode where you write a test that passes trivially because the new
  code path was never actually exercised.

* **Fixture-hex-length trap.** Cgroup regexes are anchored to
  EXACTLY `{64}` hex chars. Test fixtures that build a 64-hex CID
  by concatenating short tokens (`"abc123" + "def456" + ...`) are
  easy to get wrong — once we shipped a fixture with 88 chars
  pretending to be 64. Always use `QString(64, QLatin1Char('c'))`
  for predictable length; reach for concatenation only when the
  test specifically needs distinguishing prefixes (e.g. the
  chain-depth test in `test_cgroup_parse.cpp::chainCapsAtMaxDepth`
  uses `%1` + zero-padding to keep IDs distinct yet exactly 64
  chars).

* **What's worth testing here:** pure helpers — heuristics, aggregators,
  `Settings` migration on the `QSettings` ini path, `Exporter` formatting,
  anything in `util/` with no Qt-widget dependency. Widget regressions that
  need the real object graph belong in `test_mainwindow_smoke`, which links
  `qiftop_ui` and runs offscreen with fake monitors.

* **Hermetic QSettings:** for any test touching `Settings`, redirect
  the ini path into a `QTemporaryDir` before constructing it. See
  `tests/test_settings_migration.cpp`:
  ```cpp
  QCoreApplication::setOrganizationName("qiftop-test");
  QCoreApplication::setApplicationName("qiftop-test");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
  ```
  `QStandardPaths::setTestModeEnabled(true)` is fine for tests that do not
  manually set XDG variables. Do **not** use it in tests that override
  `XDG_CONFIG_HOME` (for example `test_autostart`) — Qt redirects generic
  config lookups to its own sandbox and defeats the manual override.

* **Qt link gotcha:** anything that includes `<QHostAddress>` (e.g.
  `ConnectionHeuristics.h`) needs `Qt6::Network` even if the test
  itself never instantiates a socket — `QHostAddress` lives in
  QtNetwork. `qiftop_add_test()` already links it.

* **Integration tests:** `QIFTOP_BUILD_INTEGRATION_TESTS=ON` (default) adds
  `test_agent_integration`, which spawns the built `qiftop-agent --session`
  and drives the live DBus contract. Run the whole suite the CI way:
  ```bash
  QT_QPA_PLATFORM=offscreen dbus-run-session -- \
      ctest --test-dir build --output-on-failure --no-tests=error
  ```
  To run one test:
  ```bash
  ctest --test-dir build -R test_filter --output-on-failure
  ```

* **Tier-2 attribution integration:** `QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON`
  adds `qiftop-attribution-probe` plus `attribution_*` CTest entries labelled
  `attribution-integration`. They require root/CAP_NET_ADMIN/CAP_SYS_ADMIN and
  a container runtime, so use the driver scripts documented under
  `tests/integration/attribution/` rather than enabling them in a normal dev
  build.

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

### 5.8 Benchmarks (opt-in, `QBENCHMARK`)

Performance microbenchmarks live under `bench/`. They use Qt Test's
**`QBENCHMARK` / `QBENCHMARK_ONCE`** — **no new third-party dependency**
(deliberately not google/benchmark; `Qt6::Test` is already in the tree).
They are **opt-in and fully excluded from normal builds and the default
`ctest` run**, so distro/CI builds are unaffected.

```sh
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DQIFTOP_BUILD_BENCHMARKS=ON
cmake --build build-bench --target benchmarks -j$(nproc)
QT_QPA_PLATFORM=offscreen ./build-bench/bench/bench_connection_aggregator
```

**Always benchmark an optimized build.** `Release` gives `-O3`; an
unoptimized (`Debug`) build produces meaningless numbers. Add
`-DQIFTOP_ENABLE_LTO=ON` for the most representative release-grade figures.

Conventions (mirror `tests/`):
* Each `bench/bench_*.cpp` is its own executable via `qiftop_add_benchmark()`
  and, like the tests, **compiles the specific `src/` files it needs
  directly** rather than linking `libqiftop` — keeps the bench build
  isolated and fast.
* The targets are `EXCLUDE_FROM_ALL`; their CTest entries are registered
  with `DISABLED TRUE` and the `benchmark` label, so a plain `ctest` never
  runs them (verify: `ctest -N` shows them as `(Disabled)`; a build without
  the flag lists none).
* Inputs come from `bench/BenchData.h` — deterministic (seeded) synthetic
  `Connection` / `InterfaceStats` generators that scale 1k → 100k. No DNS,
  no network, no kernel I/O, so the numbers are reproducible.
* Resolver/conntrack benchmarks (sock_diag, `/proc`, cgroup) need a live
  kernel and belong in an integration tier, not these pure micro-benches.

**Baseline (golden-nugget reference — dev host, Release, single-tick cost).**
These are the empirical basis for the "how eager can attribution get?"
question (see the attribution-tuning design): the per-tick *data pipeline*
is cheap, so the agent's poll cadence — not the pipeline — is the budget.

| Path | 4096 flows | 100k flows |
|------|-----------:|-----------:|
| `ConnectionAggregator` update (no DNS/UDP) | ~8.7 ms | ~316 ms |
| `ConnectionFilter` evaluate (parse once) | ~0.03 ms | ~1 ms |
| `admitFlowTopK` (top-4096 admission) | ~0.21 ms | ~22 ms |
| **full pipeline tick** (cap → aggregate → filter) | ~9 ms | **~35 ms** |
| `toDtos` (Connection → ConnectionDto) | ~1 ms | ~31 ms |
| `fromDtos` (ConnectionDto → Connection) | ~1 ms | ~33 ms |

Takeaways: the **aggregator dominates** and is the thing to watch when
raising flow caps; filtering and top-K are nearly free; even at 100k flows
the pure pipeline fits well within a 1 s tick. Re-measure on your own host
before drawing conclusions — these are relative, not absolute, guarantees.

The `bench_pipeline_tick` row is the punchline for the eagerness work: a full
data-plane tick over **100k raw flows is only ~35 ms** — versus ~316 ms to
aggregate 100k uncapped — because the **top-K cap means aggregation only ever
touches 4096 flows**; the raw count only flows through the cheap cap scan. So
the cap is exactly what keeps "eager" cadence safe: the poll interval, not the
pipeline, is the ceiling.

`bench_dbus_types` baselines the wire **conversion** layer (`toDtos`/`fromDtos`)
that the agent and every client run per snapshot — ~1 ms at the 4096-flow cap,
~30 ms at 100k. It's our code (22 fields incl. the nested container chain), so
it's the number to watch before the v0.4 async `AttributionChanged` patch
signal starts re-converting refined rows. The actual QtDBus
marshal/demarshal needs a live bus and is an integration-tier concern (a
`QDBusArgument` can't even be marshalled into standalone in-process — it
aborts), so it's deliberately out of this pure-microbench set.

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
`probeAgent()` helper in `src/main.cpp` calls `GetInterfaces` with a
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

### Stale package after `cpack`

The auto-package `add_custom_command(POST_BUILD)` is wired to the
`qiftop-agent` target only — client-only edits (e.g. anything in
`src/ui/`) build & link the new `qiftop` binary but **do not** trigger
a package regen, so reinstalling `build/qiftop_*_amd64.deb` can keep
installing the *previous* client. Symptom: edits seem to have no effect
at runtime even though the build succeeded.

Two fixes:

```bash
# Force a repackage by hand:
(cd build && cpack -G DEB)
sudo apt install ./build/libqiftop0_*.deb ./build/qiftop_*.deb

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
      rules, rebuild and inspect affected `.deb`/`.rpm` component packages.
- [ ] If you added a DBus method, removed one, or changed a signature:
      * AGENTS.md §4 table updated.
      * `NetworkAgent` interface name bumped if it was breaking.
- [ ] If you added a config key:
      * `dist/conf/agent.conf` has a top-comment block for it.
      * `loadIdleConfig()` (or successor) reads it with a sensible default.
- [ ] If you made a major change or refactor:
      * AGENTS.md updated where its architecture/contract/convention text changed.
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
* **`release.yml`** — triggered on `v*` tag push. Builds `.deb`s on
  ubuntu-24.04 and `.rpm`s in a fedora:44 container, verifies the tag
  matches `project(qiftop VERSION ...)`, computes SHA256SUMS, and publishes
  a GitHub Release with all component packages attached. Release notes are
  auto-generated by GitHub (diff vs. the previous tag), with category grouping
  configured in `.github/release.yml`. Tags containing `-` (e.g. `v0.2-rc1`)
  are marked as prerelease.
* **`pages.yml`** — triggered by `release: published` or manually. Downloads
  all release `.deb`/`.rpm` assets, runs `dist/repo/build-pages.sh`, and
  deploys signed apt + dnf repos to GitHub Pages. Releases created by
  `GITHUB_TOKEN` do not fire `release: published`, so after a tag-driven
  release run it manually once:
  ```bash
  gh workflow run pages.yml
  ```

To cut a release:

```bash
# 1. Bump project version (single source of truth — packages and
#    runtime --version derive from it).
vim CMakeLists.txt                       # project(qiftop VERSION X.Y ...)
vim AGENTS.md                            # update any changed contract/convention docs
vim HACKING.md                           # bump Last-reviewed line

# 2. Commit + tag + push.
git commit -am "release: X.Y"
git tag -a vX.Y -m "qiftop X.Y"
git push origin master vX.Y
```

The `release.yml` workflow will refuse to publish if `CMakeLists.txt`'s
`project()` VERSION doesn't match the tag base (`vX.Y-rc1` → `X.Y`).
That's deliberate — keeps the package versions, the GitHub release tag, and
`qiftop --version` consistent.

### Manual fallback

If GitHub Actions is unavailable, the same steps locally:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQIFTOP_AUTO_PACKAGE=OFF
cmake --build build --parallel
(cd build && cpack -G DEB)
```

Build RPMs with the Fedora container recipe in §2, then combine checksums:

```bash
(cd build && sha256sum *.deb > SHA256SUMS.deb)
(cd build-rpm && sha256sum *.rpm > SHA256SUMS.rpm)
cat build/SHA256SUMS.deb build-rpm/SHA256SUMS.rpm > SHA256SUMS
```

Smoke-install on a clean Ubuntu container (same shape as CI):

```bash
docker run --rm -v "$PWD/build:/work" -w /work ubuntu:24.04 bash -euxc '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq ./libqiftop0_*.deb ./qiftop-agent_*.deb ./qiftop_*.deb ./nqiftop_*.deb
  QT_QPA_PLATFORM=offscreen qiftop --version
  qiftop-agent --version
  nqiftop --version
  getent group netdev
'
```

Then upload the `.deb`s, `.rpm`s, and `SHA256SUMS` manually via the GitHub
Releases UI, and run `gh workflow run pages.yml` to refresh the apt/dnf repos.

---

_Last reviewed: 2026-06-10._
