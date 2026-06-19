# conntrack-vs-BPF/pcap capture measurement harness

**Measure-first tooling.** Before we ever build a libpcap/eBPF capture datapath
for the Linux backend, we measure — against independent ground truth — whether
it would actually beat the current conntrack path, and by how much. Full design
and the explicit list of methodological gaps it closes:
`DESIGN.md` (co-located).

This directory is **Phase 0: the conntrack baseline**. It is **opt-in, VM-only,
never packaged**, and adds **zero runtime package dependencies** (the collector
reuses the agent's existing `libnetfilter_conntrack` dep; generator + sink are
plain sockets; scorer is Python).

## What's here

| File | Role |
|------|------|
| `gen.cpp` → `qiftop-bpfeval-gen` | Ground-truth generator: forks many worker PIDs that open known flows and log every flow authoritatively (5-tuple, pid+starttime, true direction, bytes, open/close ms). |
| `sink.cpp` → `qiftop-bpfeval-sink` | epoll TCP/UDP sink that drains/echoes so flows complete (runs in the peer netns). |
| `conntrack_collect.cpp` → `qiftop-bpfeval-conntrack` | Runs the **production** `ConntrackMonitor` and dumps every snapshot's flows as NDJSON (absolute `CLOCK_MONOTONIC` timestamps). |
| `score.py` | Path-agnostic scorer: ground truth ⋈ snapshots → coverage / direction / attribution / latency. |
| `run-bpf-eval.sh` | Orchestrator: builds a "remote peer" netns (so direction is measurable), runs sink+collector+generator, scores. Skip-safe. |

## Build

```bash
cmake -S . -B build -DQIFTOP_BUILD_BPF_EVAL=ON
cmake --build build --target qiftop-bpfeval-gen qiftop-bpfeval-sink qiftop-bpfeval-conntrack
# binaries + scripts land in build/bpf-eval/
```

The option defaults **OFF**, so a normal build is unaffected. When ON, any
missing prerequisite is skipped with a `STATUS` message rather than failing
configure.

## Run (VM, as root)

```bash
sudo build/bpf-eval/run-bpf-eval.sh S2   # S1 steady | S2 churn/short | S3 many-PID
```

It prints a scorecard:

```
flow coverage      : 88.0%  (subpoll 41.0% | multipoll 100.0%)
direction          : 99.0% correct | 0.0% wrong | 1.0% unknown
attribution        : 95.0% correct | 4.0% missed | 1.0% wrong
capture latency    : p50=620 ms  p95=1180 ms
```

The headline Phase-0 question is **subpoll flow coverage** — how many short
flows the 1 s conntrack dump structurally misses. Those numbers fix the
build/don't-build thresholds in the design doc §1.

## Why a peer netns?

On a single host every flow is host↔host, so direction is inherently ambiguous
(both ends local → the heuristic returns `Unknown`). The orchestrator puts the
sink in a separate netns reached over a veth `/24`, so from the host's
conntrack the flow has a genuinely non-local remote and direction is a real,
scorable quantity. (Design doc §3.)

## Not in this phase

pcap and eBPF collectors (Phases 1–2), the hybrid (Phase 3), the S4
forwarded/router and S5 container scenarios, and load sweeps. They slot in
behind the same generator + scorer + schema — see the design doc.
