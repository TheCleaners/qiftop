# libqiftop examples

Standalone **libqiftop** consumers. Each links only `qiftop::qiftop` (Qt6
Core/Network/DBus, no Widgets) and streams from `qiftop-agent` over DBus.

| Example | Binary | What it shows |
|---------|--------|---------------|
| [`ndjson-stream`](ndjson-stream) | `qiftop-ndjson` | Interface snapshots as NDJSON. |
| [`ndjson-connections`](ndjson-connections) | `qiftop-ndjson-connections` | Flow NDJSON with process/container attribution. |
| [`prometheus-exporter`](prometheus-exporter) | `qiftop-exporter` | Prometheus `/metrics` endpoint. |
| [`snapshot-export`](snapshot-export) | `qiftop-snapshot-export` | One-shot CSV/JSON export. |
| [`top-talkers`](top-talkers) | `qiftop-top` | Headless `iftop -t`-style top-N output. |

## Building

Install libqiftop development files (`libqiftop-dev` / `qiftop-devel`), then
build any example as a standalone CMake project:

```bash
cd <example>
cmake -S . -B build -DCMAKE_PREFIX_PATH=/usr   # or your libqiftop prefix
cmake --build build
./build/<binary> --help-ish-flags
```

Common flag: `--session` uses a development agent on the session bus instead of
the system bus. See each example's source header for its flags.

## Writing your own

Call `qiftop::dbus::registerTypes()` once, feed a `DBus*Monitor` into an
`InterfaceAggregator` or `ConnectionAggregator`, then serialise `rows()`. See
[`docs/LIBQIFTOP.md`](../docs/LIBQIFTOP.md) for the full guide.
