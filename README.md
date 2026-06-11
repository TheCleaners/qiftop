# <img src="docs/logo.png" width="32" align="top" alt="qiftop logo"> qiftop

[![CI](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml)
[![Release](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt](https://img.shields.io/badge/Qt-6-41cd52)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-orange)

Qt6 iftop-style network monitor for Linux.

`qiftop` is a Qt 6 GUI that visualises per-interface byte/packet counters
(via libnl-route-3) and per-connection flow accounting (via
libnetfilter_conntrack), with optional per-flow **process and container
attribution**. Privileged data collection is split out into a small DBus
system-bus daemon (`qiftop-agent`) so the UI itself does not need elevated
capabilities. A terminal front-end, **`nqiftop`**, offers the same views
over SSH / on headless hosts (ncurses, no X11/Wayland).

![qiftop grouping live synthetic traffic by process and touring the
preferences dialog](https://github.com/TheCleaners/qiftop/releases/download/v0.2-rc1/demo.gif)

![nqiftop showing connections grouped by container with the row gauge, the
settings panel and live theme switching](https://github.com/TheCleaners/qiftop/releases/download/v0.2-rc1/nqiftop-demo.gif)

> Both captures are driven entirely by **synthetic data** (reserved
> documentation addresses + `example.*` hostnames) — see
> [`docs/demo/`](docs/demo/). They are hosted as release assets to keep the
> repo lean.

## Features

- **Per-interface counters** with sortable RX/TX byte and packet rates.
- **Per-connection flow accounting** for TCP, UDP, ICMP/ICMPv6 — IPv4
  and IPv6 first-class. Each row is a 5-tuple with directionality
  (inbound / outbound / unknown), aggregated by peer for UDP, and
  optionally tinted by direction.
- **Process & container attribution** — every flow can carry the owning
  PID / `comm` / UID plus the container runtime, id and name (Docker,
  containerd, Podman, CRI-O, Kubernetes, LXC/LXD, systemd-nspawn),
  including the full nested container chain. Group the Connections view
  by interface, container or process; group headers show colour-coded
  PID / user / container detail chips.
- **Live throughput gauge** drawn under each row, with adaptive
  reference (sliding-window peak or cumulative average) and optional
  smoothed display rates (EMA + easeOutCubic tween between polls).
- **Filter expression mini-language** for the Connections view:
  `proto:tcp and dport=443`, `iface=wlp228s0 and rate>1Mi`,
  `host~"\.google\.com"`, `container:nginx`, `comm=postgres`, etc.
  Booleans, numeric comparisons, byte suffixes (`K/Ki/M/Mi/...`), regex.
- **Async DNS** with in-process cache; addresses are rendered as
  hostnames where possible without blocking the UI.
- **System tray** with live rate sparklines and an optional
  "start on login (silently into tray)" XDG autostart entry.
- **Privilege split**: the unprivileged GUI talks to `qiftop-agent`
  (running with `CAP_NET_ADMIN`) over the system bus, with a
  self-elevation fallback path for machines where the agent isn't
  installed.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Build dependencies (Debian/Ubuntu)

```sh
sudo apt install \
    cmake g++ pkg-config \
    qt6-base-dev qt6-base-dev-tools \
    libnl-3-dev libnl-route-3-dev \
    libnetfilter-conntrack-dev
```

## Install from the package repository (recommended)

Signed **apt** (Debian/Ubuntu) and **dnf** (Fedora) repositories are
hosted at <https://thecleaners.github.io/qiftop/>.

**Debian / Ubuntu:**

```sh
curl -fsSL https://thecleaners.github.io/qiftop/qiftop-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/qiftop-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/qiftop-archive-keyring.gpg] https://thecleaners.github.io/qiftop/deb stable main" \
  | sudo tee /etc/apt/sources.list.d/qiftop.list
sudo apt update && sudo apt install qiftop qiftop-agent
```

**Fedora:**

```sh
sudo curl -fsSL https://thecleaners.github.io/qiftop/rpm/qiftop.repo \
  -o /etc/yum.repos.d/qiftop.repo
sudo rpm --import https://thecleaners.github.io/qiftop/qiftop-archive-keyring.asc
sudo dnf install qiftop qiftop-agent
```

Both repos are GPG-signed (key
`7AC658ABFADD1AAF6E0EDA6F6DD33D47032BD42D`). The dnf repo uses the
metadata-signature trust model (`repo_gpgcheck=1`): the signed
`repomd.xml` authenticates package checksums, exactly like apt's signed
`Release`. Per-package RPM signatures (`gpgcheck=1`) are a planned
post-stable addition. Don't have repo access? Grab a `.deb`/`.rpm`
directly from the
[releases page](https://github.com/TheCleaners/qiftop/releases).

## Install from a local build

```sh
sudo cmake --install build
# the agent activates on demand over DBus, or:
sudo systemctl start qiftop-agent
```

## Building the packages yourself (.deb / .rpm)

CPack produces two packages — one for the daemon (+ systemd / DBus
units) and one for the GUI. Debian/Ubuntu (`.deb`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && cpack -G DEB
ls *.deb
# qiftop-agent_<ver>_amd64.deb
# qiftop_<ver>_amd64.deb
```

Fedora (`.rpm`) — requires `rpm-build` and the Fedora `-devel` libs, so
build it on Fedora (or in a `fedora` container; see
[`dist/rpm/build-and-verify.sh`](dist/rpm/build-and-verify.sh)):

```sh
cd build && cpack -G RPM
ls *.rpm
# qiftop-agent-<ver>-1.fc44.x86_64.rpm
# qiftop-<ver>-1.fc44.x86_64.rpm
```

Library dependencies are resolved automatically (rpm find-requires /
`dpkg-shlibdeps`); `qiftop` depends on `qiftop-agent` only via a weak
`Recommends:`, so the GUI still runs (with reduced functionality and a
"Relaunch as administrator" fallback) on machines without the agent
installed. Prebuilt `.deb` and `.rpm` assets are attached to each
[GitHub Release](https://github.com/TheCleaners/qiftop/releases).

## Contributing & internals

- [`docs/HACKING.md`](docs/HACKING.md) — developer cookbook: build/run/debug
  recipes, common dev tasks, debugging gotchas.
- [`AGENTS.md`](AGENTS.md) — architecture reference: layering rules,
  DBus contract, config keys, testability notes.
- [`docs/CHANGELOG.md`](docs/CHANGELOG.md) — release notes (Keep a Changelog
  format).
- [`SECURITY.md`](SECURITY.md) — how to report vulnerabilities
  privately (the agent runs as root; this matters).

## License

`qiftop` is licensed under the **GNU General Public License v2.0 or
later** — see [`LICENSE`](LICENSE). This is the strictest license among
its runtime dependencies (`libnetfilter_conntrack` is GPL-2.0) and is
compatible with Qt 6 (LGPL-3.0) and libnl-3 (LGPL-2.1) under dynamic
linkage.
