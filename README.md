# <img src="docs/logo.png" width="32" align="top" alt="qiftop logo"> qiftop

[![CI](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml)
[![Release](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt](https://img.shields.io/badge/Qt-6-41cd52)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-orange)

Qt6 iftop-style network monitor for Linux, with desktop, terminal and library
entry points.

`qiftop` is built as four cooperating components:

- **`qiftop`** — the Qt 6 Widgets GUI client.
- **`nqiftop`** — an ncurses TUI for SSH/headless use, sharing the same data
  path and settings concepts.
- **`qiftop-agent`** — a privileged DBus daemon on the system bus
  (`org.qiftop.NetworkAgent1`), running as root with a bounded capability set,
  that collects per-interface stats with libnl-route-3 and per-flow stats with
  libnetfilter_conntrack, including server-side process/container attribution.
- **`libqiftop`** — a Widgets-free Qt shared library (`qiftop::qiftop`) with
  the DBus DTOs/client proxies, aggregators, filter mini-language, IEC unit
  formatters and JSON/CSV export helpers for external consumers.

![qiftop grouping live synthetic traffic by process and touring the
preferences dialog](https://github.com/TheCleaners/qiftop/releases/download/v0.2-rc1/demo.gif)

![nqiftop showing connections grouped by container with the row gauge, the
settings panel and live theme switching](https://github.com/TheCleaners/qiftop/releases/download/v0.2-rc1/nqiftop-demo.gif)

> Both captures are driven entirely by **synthetic data** (reserved
> documentation addresses + `example.*` hostnames) — see
> [`docs/demo/`](docs/demo/). They are hosted as release assets to keep the
> repo lean.

## Features

- **Per-interface counters** with sortable RX/TX byte and packet rates, link
  state, addresses, MTU and error/drop counters.
- **Per-connection flow accounting** for TCP, UDP, ICMP/ICMPv6 — IPv4 and IPv6
  first-class — with directionality, TCP state, totals and live rates.
- **Process & container attribution** — flows can carry PID / `comm` / UID,
  container runtime/id/name and the full outer→inner container chain (Docker,
  containerd, Podman, CRI-O, Kubernetes, LXC/LXD, systemd-nspawn).
- **Grouped Connections views** in the GUI and TUI: flat/off, by interface, by
  process or by container; group rows aggregate child rates and totals.
- **Filter expression mini-language** shared by GUI, TUI and libqiftop:
  `proto:tcp and dport=443`, `iface:wlp* and rate>1Mi`, `comm=postgres`,
  `container:nginx`, `chain_has:kubernetes`, `pid=0`, with boolean operators,
  numeric comparisons, byte suffixes and regex.
- **Live row gauges and smoothing**: row-spanning throughput gauges, direction
  tinting, UDP peer aggregation and optional EMA/eased display rates.
- **Async reverse DNS** with an in-process cache; hostnames never block the UI.
- **`nqiftop` for terminals**: `j`/`k` and arrow navigation, `Enter`/`l`
  modal detail, `g` grouping, `/` filters, `p` true snapshot freeze, `w`
  timestamped CSV export, `z` themes, `S` settings, `1`/`2` tabs, `?` help,
  `a` about.
- **Reusable data library and examples**: standalone `find_package(qiftop)`
  consumers live in [`examples/`](examples/) — see
  [`examples/README.md`](examples/README.md) for NDJSON, Prometheus, snapshot
  export and top-talkers examples.
- **Privilege split**: unprivileged clients talk to `qiftop-agent`; DBus access
  is gated to `root`/`netdev`, and `/etc/qiftop/agent.conf` controls process
  detail disclosure.

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

The repositories are signed with the qiftop package signing key
`7AC658ABFADD1AAF6E0EDA6F6DD33D47032BD42D` (the landing page may show the
short key id `6DD33D47032BD42D`). The dnf repo enables both signed metadata
(`repo_gpgcheck=1`) and per-package RPM signatures (`gpgcheck=1`).

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

Library dependencies are resolved automatically (`dpkg-shlibdeps` /
rpm find-requires). `qiftop` and `nqiftop` weakly recommend `qiftop-agent`, so
they can still fall back to in-process capture when the agent is unavailable.

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
