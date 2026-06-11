# libqiftop — the qiftop data facility

`libqiftop` is the Qt6::Core-only shared library that carries everything
qiftop knows about network traffic *except the GUI*. It exists so the same
data plane can feed many frontends and tools — not just the Qt app.

It ships as two packages:

| Package (deb / rpm)              | Contents                                            |
|----------------------------------|-----------------------------------------------------|
| `libqiftop0` / `qiftop-libs`     | the runtime `.so` (SONAME `libqiftop.so.0`)         |
| `libqiftop-dev` / `qiftop-devel` | headers, CMake config, pkg-config                   |

No Qt Widgets dependency, so it runs headless (over SSH, in a container, in
a daemon). `qiftop` (GUI) and `qiftop-agent` both link it.

## The data plane

```
                    ┌─────────────── libqiftop ───────────────┐
 qiftop-agent  ──DBus──▶  DBusNetworkMonitor    InterfaceAggregator ─┐
 (system bus)            DBusConnectionMonitor   ConnectionAggregator │
                              (source)              (batch + stream)  │
                                                        │             │
                          util/ConnectionFilter  ◀──────┤        consumer
                          util/Units (IEC fmt)          │        (your code)
                          util/Exporter (JSON/CSV) ◀────┘             │
                          dbus/Types (DTOs / wire) ───────────────────┘
```

* **Source** — `DBus{Network,Connection}Monitor` subscribe to the agent's
  `org.qiftop.NetworkAgent1` signals and re-emit `statsUpdated` /
  `connectionsUpdated`. (The privileged in-process capture path —
  `backend/linux` — is *not* shipped in the lib; consumers stream from the
  agent. That keeps libqiftop unprivileged and platform-agnostic.)
* **Aggregators** — `InterfaceAggregator` / `ConnectionAggregator` are plain
  `QObject`s that own the derived view: per-interface and per-flow rates,
  the 3-layer EMA rate-smoothing pipeline, UDP peer-aggregation, direction
  inference, the adaptive throughput reference, DNS hostnames, stale-flow
  retention. They are simultaneously:
    - **batch** — query `rows()` / `rowAt(i)` for the current snapshot;
    - **stream** — connect to their change signals (`rowsChanged`,
      `rowsUpdated`, `rowsInserted`/`Removed`, `didReset`) for deltas.
* **Filter** — `qiftop::filter` (the `proto:tcp and rate_total>1M`
  mini-language) evaluates against a `Context` built from a `Connection`,
  so any consumer can filter without re-implementing the grammar.
* **Format** — `util::Exporter` serialises any `Exportable` (the models, or
  your own) to JSON / CSV with the security hardening (CSV formula
  injection, quint64-as-string) baked in. `util::formatBytes` /
  `formatByteRate` give the IEC unit strings.
* **DTOs** — `dbus/Types.h` is the wire contract; consumers that talk to
  the agent directly (or replay captures) use the same structs.

## Wiring a consumer

The whole pattern is "source → aggregator → serialise", on a
`QCoreApplication` event loop. See `examples/ndjson-stream/` for a ~100-line
consumer that turns the agent's live stream into NDJSON on stdout:

```cpp
auto *mon = new DBusNetworkMonitor(/*session=*/false, &app);
auto *agg = new InterfaceAggregator(&app);
QObject::connect(mon, &NetworkMonitor::statsUpdated,
                 agg, &InterfaceAggregator::updateStats);
QObject::connect(agg, &InterfaceAggregator::rowsChanged, &app, [agg]{
    /* serialise agg->rows() to NDJSON / Prometheus / an alert rule */
});
mon->start();
```

Build against it with either:

```cmake
find_package(qiftop REQUIRED)
target_link_libraries(my_tool PRIVATE qiftop::qiftop)
```

or `pkg-config --cflags --libs qiftop`.

## Target use cases

The facility is designed so each of these is a thin consumer, not a fork.
Shipped examples live under [`examples/`](../examples) (standalone
`find_package(qiftop)` projects):

1. **NDJSON / line-oriented CLI** — `examples/ndjson-stream` (interfaces) and
   `examples/ndjson-connections` (per-flow, with process/container
   attribution). Pipe live data into `jq`, a log shipper, `awk`, etc.;
   `--once` gives a batch snapshot for cron/scripts.
2. **Prometheus / OpenMetrics exporter** — `examples/prometheus-exporter`:
   serves `/metrics` (interface byte counters + per-container/process rate
   gauges) so alerting ("incessantly hogging" via a PromQL `for:` clause) is
   delegated to Prometheus/Alertmanager — no new ecosystem to learn.
3. **Snapshot export** — `examples/snapshot-export`: one-shot CSV/JSON dump of
   the current flow table via `util::exporter`.
4. **Top-talkers CLI** — `examples/top-talkers`: a headless `iftop -t`-style
   printer over the aggregator + IEC rate formatter.
5. **Alerting daemon** (planned) — evaluate the filter mini-language against
   the stream; fire on `rate_total>X`, a new container talking out, etc. See
   the v0.3 alerting design.
6. **Language bindings** — Python / Rust over a future stable C ABI
   (`extern "C"` shim) wrapping the DTOs + aggregators. Deferred, but the
   library is kept binding-friendly (plain value types, no Widgets).
7. **Embedding** — any Qt app that wants live traffic data without the
   qiftop UI links the aggregators directly.
8. **Alternative frontends** — the ncurses `nqiftop` is the first
   non-Qt-Widgets frontend; it reuses the aggregators wholesale.

## Stability

Pre-1.0 the ABI is **unstable** — the CMake `find_package` config requires
an *exact* version match, and the SONAME is `libqiftop.so.0`. The durable
cross-process contract is the **DBus wire format** (`org.qiftop.NetworkAgent1`,
see `AGENTS.md §4`), not the C++ ABI. Treat the C++ API as "recompile your
consumer against each qiftop release" until 1.0.

## Future: `libqiftop-core`

The pure-logic subset (DTOs, the filter mini-language, unit formatters, the
aggregation/EMA helpers) has no Network/DBus dependency and is kept in a
self-contained seam so it can graduate to a separate `libqiftop-core`
package if a consumer wants the logic without the DBus client (e.g. a
`/proc`-reading exporter, or pure bindings). Not split today — the current
consumers all want the full source→aggregate→format stack.
