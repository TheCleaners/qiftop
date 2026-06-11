# qiftop Prometheus / OpenMetrics exporter (example)

A ~250-line example consumer of **libqiftop** that exposes the qiftop-agent's
live network stats as a Prometheus-scrapeable `/metrics` endpoint. It links
only `qiftop::qiftop` (Qt6 Core/Network/DBus) — no Qt Widgets, no kernel
access — so it runs headless on a server.

This is the "integration-first" answer to *"is a container incessantly
hogging my bandwidth?"*: qiftop is the data source; **Prometheus +
Alertmanager** do the alerting (rules, dedup, routing, silencing, on-call),
so sysadmins reuse the stack they already run instead of learning a new one.

## Build

Requires an installed libqiftop (`libqiftop-dev` / `qiftop-devel`):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr   # or wherever libqiftop is installed
cmake --build build
```

## Run

```bash
./build/qiftop-exporter --port 9617 &
curl -s localhost:9617/metrics | grep qiftop_container
```

Flags: `--session` (dev session bus), `--port <n>` (default 9617),
`--bind <addr>` (default 0.0.0.0), `--interval-ms <n>` (agent cadence +
heartbeat base, default 1000). The exporter heartbeats the agent so it does
not wind down to idle.

Scrape config:

```yaml
scrape_configs:
  - job_name: qiftop
    static_configs:
      - targets: ['host:9617']
```

## The motivating alert: a container hogging bandwidth

`for:` is exactly "incessantly" — sustained, not a spike. The container rate
is a **gauge** qiftop computes itself, so use `max_over_time` / `avg_over_time`
over a window (not `rate()`, which is for monotonic counters):

```yaml
groups:
  - name: qiftop
    rules:
      - alert: ContainerBandwidthHog
        expr: max_over_time(qiftop_container_tx_rate_bytes{runtime="docker"}[5m]) > 50e6
        for: 2m
        labels: { severity: warning }
        annotations:
          summary: "{{ $labels.container }} ({{ $labels.runtime }}) egress > 50 MB/s for 2m"

      # Share of CURRENT interface throughput (no link-capacity guesswork):
      - alert: ContainerDominatesEgress
        expr: |
          sum by (container) (qiftop_container_tx_rate_bytes{runtime="docker"})
            / on() group_left() sum(qiftop_interface_tx_rate_bytes{iface="eth0"})
            > 0.6
        for: 5m
```

> Note: we deliberately do **not** alert on a "% of NIC link speed" — link/PHY
> speed often differs from provisioned bandwidth (switch policing, QoS, ISP/CIR
> caps, cloud egress caps; a 10G NIC capped to 2G via switching). Alert on
> observed interface throughput, or hard-code your provisioned capacity.

## Metrics

| Metric | Type | Labels | Notes |
|--------|------|--------|-------|
| `qiftop_interface_rx_bytes_total` / `_tx_bytes_total` | counter | `iface` | True monotonic kernel counters; safe for `rate()`. |
| `qiftop_interface_rx_rate_bytes` / `_tx_rate_bytes` | gauge | `iface` | Current per-interface rate (B/s). |
| `qiftop_interface_up` | gauge | `iface` | 1 = administratively up. |
| `qiftop_container_rx_rate_bytes` / `_tx_rate_bytes` | gauge | `runtime`, `container` | Aggregated raw rate per container/scope (B/s). |
| `qiftop_container_active_flows` | gauge | `runtime`, `container` | Active flows attributed to the container. |
| `qiftop_process_rx_rate_bytes` / `_tx_rate_bytes` | gauge | `comm`, `uid` | Aggregated raw rate per process. |

### Why per-container rates are gauges, not counters

Per-flow cumulative byte counters are a Prometheus trap: ephemeral ports make
the label cardinality explode, and a sum over *currently-open* flows is not
monotonic (it drops when a flow closes), which corrupts `rate()`. qiftop
already computes rates from raw deltas, so the exporter ships those as gauges
and lets PromQL window them with `avg_over_time` / `max_over_time` — the
correct idiom for an externally-computed rate. Cardinality stays bounded to
the number of containers/processes, not the number of 5-tuples.

Attribution labels (`container`, `runtime`, `comm`, `uid`) are populated only
when the agent advertises the attribution capability tokens; otherwise the
container/process families are empty and the interface metrics still work.
