# qiftop

[![CI](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/ci.yml)
[![Release](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml/badge.svg)](https://github.com/TheCleaners/qiftop/actions/workflows/release.yml)
![Tests](https://img.shields.io/badge/tests-16%20passing-brightgreen)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Qt](https://img.shields.io/badge/Qt-6-41cd52)

Qt6 iftop-style network monitor for Linux.

`qiftop` is a Qt 6 GUI that visualises per-interface byte/packet counters
(via libnl-route-3) and per-connection flow accounting (via
libnetfilter_conntrack). Privileged data collection is split out into a
small DBus system-bus daemon (`qiftop-agent`) so the UI itself does not
need elevated capabilities.

![qiftop showing live per-connection traffic with the throughput gauge
and the filter expression bar](screenshot1.png)

## Features

- **Per-interface counters** with sortable RX/TX byte and packet rates.
- **Per-connection flow accounting** for TCP, UDP, ICMP/ICMPv6 — IPv4
  and IPv6 first-class. Each row is a 5-tuple with directionality
  (inbound / outbound / unknown), aggregated by peer for UDP, and
  optionally tinted by direction.
- **Live throughput gauge** drawn under each row, with adaptive
  reference (sliding-window peak or cumulative average) and optional
  smoothed display rates (EMA + easeOutCubic tween between polls).
- **Filter expression mini-language** for the Connections view:
  `proto:tcp and dport=443`, `iface=wlp228s0 and rate>1Mi`,
  `host~"\.google\.com"`, etc. Booleans, numeric comparisons, byte
  suffixes (`K/Ki/M/Mi/...`), regex.
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

## Install

```sh
sudo cmake --install build
# the agent activates on demand over DBus, or:
sudo systemctl start qiftop-agent
```

## Debian packaging

CPack produces two `.deb` packages — one for the daemon (+ systemd / DBus
units) and one for the GUI:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
cd build && cpack -G DEB
ls *.deb
# qiftop-agent_<ver>_amd64.deb
# qiftop_<ver>_amd64.deb
```

`qiftop` depends on `qiftop-agent` only via a `Recommends:`; the GUI
still runs (with reduced functionality and a "Relaunch as
administrator" fallback) on machines without the agent installed.

## Contributing & internals

- [`HACKING.md`](HACKING.md) — developer cookbook: build/run/debug
  recipes, common dev tasks, debugging gotchas.
- [`AGENTS.md`](AGENTS.md) — architecture reference: layering rules,
  DBus contract, config keys, testability notes.
- [`CHANGELOG.md`](CHANGELOG.md) — release notes (Keep a Changelog
  format).
- [`SECURITY.md`](SECURITY.md) — how to report vulnerabilities
  privately (the agent runs as root; this matters).

## License

`qiftop` is licensed under the **GNU General Public License v2.0 or
later** — see [`LICENSE`](LICENSE). This is the strictest license among
its runtime dependencies (`libnetfilter_conntrack` is GPL-2.0) and is
compatible with Qt 6 (LGPL-3.0) and libnl-3 (LGPL-2.1) under dynamic
linkage.
