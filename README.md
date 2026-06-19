# <img src="docs/logo.png" width="32" align="top" alt="qiftop logo"> qiftop

[![CI](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml)
[![Release](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt](https://img.shields.io/badge/Qt-6-41cd52)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20FreeBSD%20%7C%20NetBSD-555)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-orange)

Qt 6 iftop-style network monitor with desktop, terminal and library entry
points.

`qiftop` is built as four cooperating components:

- **`qiftop`** — the Qt 6 desktop GUI.
- **`nqiftop`** — an ncurses terminal UI for SSH and headless machines.
- **`qiftop-agent`** — a privileged DBus daemon that collects network data and
  serves unprivileged clients.
- **`libqiftop`** — a Widgets-free shared Qt library (`qiftop::qiftop`) for
  building your own qiftop-powered tools.

![qiftop grouping live synthetic traffic by process and touring the
preferences dialog](https://github.com/TheCleaners/qiftop/releases/download/v0.3.2/demo.gif)

![nqiftop showing connections grouped by container with the row gauge, the
settings panel and live theme switching](https://github.com/TheCleaners/qiftop/releases/download/v0.3.2/nqiftop-demo.gif)

> Both captures are driven entirely by **synthetic data** (reserved
> documentation addresses + `example.*` hostnames) — see
> [`docs/demo/`](docs/demo/). They are hosted as release assets to keep the
> repo lean.

## Features

- **Per-interface counters** with sortable RX/TX byte and packet rates, link
  state, addresses, MTU and error/drop counters.
- **Per-connection flow accounting** for TCP, UDP, ICMP/ICMPv6, with IPv4 and
  IPv6 support, direction, TCP state, totals and live rates.
- **Process & container attribution** — show the owning process and container
  when available, including nested containers across common Linux runtimes.
  Flows with no local owner are labelled as forwarded/NAT, orphaned, or
  no-local-socket instead of looking like failed lookups.
- **Grouped Connections views** in the GUI and TUI: flat/off, by interface, by
  process or by container; group rows aggregate child rates and totals.
- **Powerful filters** shared by GUI, TUI and libqiftop:
  `proto:tcp and dport=443`, `iface:wlp* and rate>1Mi`, `comm=postgres`,
  `container:nginx`, `chain_has:kubernetes`, `reason:forwarded`, `pid=0`, with
  boolean operators, numeric comparisons, byte suffixes and regex.
- **Live visual gauges** with direction tinting, sensible UDP peer grouping and
  smoothed display rates.
- **Async reverse DNS** with an in-process cache; hostnames never block the UI.
- **`nqiftop` for terminals**: `j`/`k`/arrows and `Ctrl-F`/`Ctrl-B`/`Ctrl-D`/
  `Ctrl-U`/PgUp/PgDn navigation, `Enter`/`l` modal detail, `h`/`l` group
  collapse/expand, `g` grouping, `/` filters, `p` true snapshot freeze, `w`
  timestamped CSV export (`W` prompts for a filename), `z` themes, `S`
  settings, `1`/`2` tabs, `?` help, `a` about.
- **Reusable data library and examples**: standalone `find_package(qiftop)`
  consumers live in [`examples/`](examples/) — see
  [`examples/README.md`](examples/README.md) for NDJSON, Prometheus, snapshot
  export and top-talkers examples.
- **Privilege split**: unprivileged clients talk to `qiftop-agent`; DBus access
  is gated to `root`/`netdev`, and `/etc/qiftop/agent.conf` controls process
  detail disclosure.

## Platform support

**Linux is the primary, full-featured platform.** The privileged
`qiftop-agent` is Linux-only, and the GUI/TUI clients normally stream from it
over D-Bus, with an in-process fallback when the agent is unavailable.

**FreeBSD and NetBSD are supported as client builds** (`qiftop` GUI + `nqiftop`
TUI + `libqiftop`), built from the same tree — the agent is simply not built
there. On the BSDs capture runs **in-process** and should be run privileged;
FreeBSD also reports jail ownership for jailed flows. See
[`docs/PORTABILITY.md`](docs/PORTABILITY.md) §7 for the BSD field guide and
build notes; packaging there is pkgsrc-stage (not yet a finished port).

### Support matrix

| Platform | Tested versions | `qiftop-agent` | GUI · TUI · `libqiftop` | Verified by | Status |
|----------|-----------------|:--------------:|:-----------------------:|-------------|--------|
| **Linux** (glibc) | Ubuntu 24.04 · Ubuntu 26.04 · Fedora 44 | ✅ | ✅ ✅ ✅ | CI (build + tests) | ![supported](https://img.shields.io/badge/full-brightgreen) |
| **Linux** (musl) | Alpine (latest) | ✅ | ✅ ✅ ✅ | CI (build + tests) | ![supported](https://img.shields.io/badge/full-brightgreen) |
| **FreeBSD** | 14.0 · 15.0 | ➖ | ✅ ✅ ✅ | VM (build + runtime) | ![client](https://img.shields.io/badge/client%20builds-blue) |
| **NetBSD** | 11.0 | ➖ | ✅ ✅ ✅ | VM (build + runtime) | ![client](https://img.shields.io/badge/client%20builds-blue) |

<sub>✅ built & working · ➖ not applicable (the privileged agent is Linux-only) ·
**full** = agent + clients, exercised by the GitHub Actions test suite ·
**client builds** = GUI/TUI/`libqiftop` build and run from the same tree with
in-process capture; verified on a VM but not in CI. Other BSDs (OpenBSD,
DragonFly) and other Linux distros are likely to work from source but are
not regularly tested.</sub>

## Install

### Package repository (recommended)

Signed **apt** (Debian/Ubuntu) and **dnf** (Fedora) repositories are hosted at
<https://thecleaners.github.io/qiftop/>.

**Debian / Ubuntu:**

```sh
curl -fsSL https://thecleaners.github.io/qiftop/qiftop-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/qiftop-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/qiftop-archive-keyring.gpg] https://thecleaners.github.io/qiftop/deb stable main" \
  | sudo tee /etc/apt/sources.list.d/qiftop.list
sudo apt update && sudo apt install qiftop qiftop-agent
```

Install `nqiftop` for the terminal UI and `libqiftop-dev` to build library
consumers/examples:

```sh
sudo apt install nqiftop libqiftop-dev
```

**Fedora:**

```sh
sudo curl -fsSL https://thecleaners.github.io/qiftop/rpm/qiftop.repo -o /etc/yum.repos.d/qiftop.repo
sudo rpm --import https://thecleaners.github.io/qiftop/qiftop-archive-keyring.asc
sudo dnf install qiftop qiftop-agent
```

Install `nqiftop` for the terminal UI and `qiftop-devel` to build library
consumers/examples:

```sh
sudo dnf install nqiftop qiftop-devel
```

The repositories are signed with the qiftop package signing key (fingerprint
`7AC658ABFADD1AAF6E0EDA6F6DD33D47032BD42D`).

### Direct release packages

Prebuilt `.deb` and `.rpm` artifacts are attached to each
[GitHub Release](https://github.com/TheCleaners/qiftop/releases). Download the
packages you want, then install them with dependency resolution:

```sh
# Debian / Ubuntu: GUI + agent + TUI
sudo apt install ./libqiftop0_*.deb ./qiftop-agent_*.deb ./qiftop_*.deb ./nqiftop_*.deb

# Fedora: GUI + agent + TUI
sudo dnf install ./qiftop-libs-*.rpm ./qiftop-agent-*.rpm ./qiftop-[0-9]*.rpm ./nqiftop-*.rpm
```

Add `./libqiftop-dev_*.deb` or `./qiftop-devel-*.rpm` if you need headers,
`find_package(qiftop)` support or pkg-config metadata for your own consumer.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The default Linux build produces `qiftop`, `qiftop-agent`, `libqiftop` and,
when ncursesw is available, `nqiftop`.

The build emits `compile_commands.json` for editors and language servers
(clangd, VS Code). Pass `-DQIFTOP_ENABLE_LTO=ON` to enable link-time
optimization where the toolchain supports it.

### Build dependencies (Debian/Ubuntu)

```sh
sudo apt install \
    cmake g++ pkg-config \
    qt6-base-dev qt6-base-dev-tools \
    libnl-3-dev libnl-route-3-dev \
    libnetfilter-conntrack-dev \
    libncurses-dev
```

## Install from a local build

```sh
sudo cmake --install build
# the agent activates on demand over DBus, or:
sudo systemctl start qiftop-agent
```

To use the system-bus agent as an unprivileged user, be in the `netdev` group
and start a new login session after the membership change.

## Building the packages yourself (.deb / .rpm)

CPack produces one package per component.

Debian/Ubuntu (`.deb`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && cpack -G DEB
ls *.deb
# libqiftop0_<ver>_amd64.deb
# libqiftop-dev_<ver>_amd64.deb
# nqiftop_<ver>_amd64.deb
# qiftop-agent_<ver>_amd64.deb
# qiftop_<ver>_amd64.deb
```

Fedora (`.rpm`) — requires `rpm-build` and Fedora `-devel` packages, so build
on Fedora (or see [`dist/rpm/build-and-verify.sh`](dist/rpm/build-and-verify.sh)):

```sh
cd build && cpack -G RPM
ls *.rpm
# qiftop-<ver>-1.fc44.x86_64.rpm
# qiftop-agent-<ver>-1.fc44.x86_64.rpm
# qiftop-libs-<ver>-1.fc44.x86_64.rpm
# qiftop-devel-<ver>-1.fc44.x86_64.rpm
# nqiftop-<ver>-1.fc44.x86_64.rpm
```

Package dependencies are resolved automatically. `qiftop` and `nqiftop` can
still fall back to in-process capture when the agent is unavailable.

## libqiftop examples

`examples/` contains standalone consumers that link only `qiftop::qiftop` and
stream data from the agent: `ndjson-stream`, `ndjson-connections`,
`prometheus-exporter`, `snapshot-export` and `top-talkers`. See
[`examples/README.md`](examples/README.md) and [`docs/LIBQIFTOP.md`](docs/LIBQIFTOP.md).

## Contributing & internals

- [`docs/HACKING.md`](docs/HACKING.md) — developer cookbook: build/run/debug
  recipes, common dev tasks, debugging gotchas.
- [`AGENTS.md`](AGENTS.md) — architecture reference: layering rules, DBus
  contract, config keys, testability notes.
- [`CHANGELOG.md`](CHANGELOG.md) — release notes (Keep a Changelog format).
- [`SECURITY.md`](SECURITY.md) — how to report vulnerabilities privately (the
  agent runs as root; this matters).

## License

`qiftop` is licensed under the **GNU General Public License v2.0 or later** —
see [`LICENSE`](LICENSE). This is the strictest license among its runtime
dependencies (`libnetfilter_conntrack` is GPL-2.0) and is compatible with Qt 6
(LGPL-3.0) and libnl-3 (LGPL-2.1) under dynamic linkage.
