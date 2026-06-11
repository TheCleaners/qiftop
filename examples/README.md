# libqiftop examples

Thin, standalone consumers of **libqiftop** — each links only
`qiftop::qiftop` (Qt6 Core/Network/DBus, no Widgets, no kernel access) and
streams live data from the `qiftop-agent` over DBus. They demonstrate that
the data facility is reusable outside the Qt GUI.

| Example | Binary | What it shows |
|---------|--------|---------------|
| [`ndjson-stream`](ndjson-stream) | `qiftop-ndjson` | Per-interface NDJSON snapshots → `jq` / log shippers. |
| [`ndjson-connections`](ndjson-connections) | `qiftop-ndjson-connections` | Per-flow NDJSON with process/container attribution. |
| [`prometheus-exporter`](prometheus-exporter) | `qiftop-exporter` | Prometheus `/metrics` endpoint; alerting via PromQL/Alertmanager. |
| [`snapshot-export`](snapshot-export) | `qiftop-snapshot-export` | One-shot CSV/JSON dump via `util::exporter`. |
| [`top-talkers`](top-talkers) | `qiftop-top` | Headless `iftop -t`-style top-N printer. |

## Building

Each example is a standalone CMake project built against an **installed**
libqiftop (`libqiftop-dev` / `qiftop-devel`):

```bash
cd <example>
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr   # or your libqiftop prefix
cmake --build build
./build/<binary> --help-ish-flags
```

Common flags: `--session` (talk to the agent on the session bus for dev
instead of the system bus). See each example's source header for its flags.

## Writing your own

The pattern is always: a `DBus*Monitor` feeds an aggregator
(`InterfaceAggregator` / `ConnectionAggregator`); you read `agg->rows()` and
serialise. One gotcha: call `qiftop::dbus::registerTypes()` once at startup,
or the agent's DBus replies fail to unmarshal. See
[`docs/LIBQIFTOP.md`](../docs/LIBQIFTOP.md) for the full architecture.
