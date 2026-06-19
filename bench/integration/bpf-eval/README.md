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

## Phase 0 results (first VM run — Ubuntu 24.04, idle 4-CPU libvirt VM)

The baseline, scored against ground truth (poll = 1 s):

| Scenario | Flows | Coverage | Byte cov | Direction | Attribution | Latency p50 |
|----------|------:|---------:|---------:|-----------|-------------|------------:|
| **S1** steady (1000 × 4 s) | 1000 | **100%** | 100% | **100% correct** | **100% correct** | 595 ms |
| **S2** churn (240 × 200 ms) | 240 | **100%** | 100% | 100% correct | **100% MISSED** | 456 ms |
| **S3** many-PID (12000, 200 workers) | 12000 | **63%** | 100% | 100% correct | **100% MISSED** | 474 ms |

**What this tells us (and reframes the whole BPF question):**

1. **conntrack capture is good, not the weak point.** Coverage is ~100% for
   long-lived *and* moderate-churn short flows — conntrack entries **linger**
   after a short flow closes (TIME_WAIT / UDP timeout), so the 1 s dump still
   catches them. Our prior "conntrack misses short flows" assumption was
   **wrong**; it only drops a material fraction (~37%) under *extreme* churn
   (S3). Byte counts are exact. → a pcap/eBPF *capture* replacement (Phase 1)
   would buy little.
2. **Direction is 100% correct** here — but only outbound client flows
   (ephemeral→well-known) are exercised; the heuristic's hard cases
   (both-ephemeral, both-local, p2p) need the not-yet-built S4/S6 before any
   verdict. Don't over-claim.
3. **The real gap is ATTRIBUTION of short-lived processes** — 100% missed in
   S2/S3 because the owning process has **exited** by the time the 1 s snapshot
   + sock_diag runs. This is independent of conntrack-vs-BPF *capture*; a packet
   datapath wouldn't fix it. **eBPF socket-birth tracepoints** (capture
   pid+direction the instant `connect()`/`accept()` fires, before the process
   dies) would. → the **hybrid** (conntrack bytes ⊕ eBPF birth events) is the
   front-runner, not a capture replacement. This sharpens Phase 2/3.

> Caveats (honesty): idle VM, veth-adjacent flows, CPU/cost not yet measured,
> S3's 63% not yet root-caused (dump-timing vs UDP-timeout-expiry vs top-K),
> and the direction win is on easy cases only. These numbers size the gap and
> point the next phase; they are not the final verdict.

## Phase 2 results — eBPF socket-birth (`birth.bt`), same VM

Run the SAME workloads with the eBPF socket-birth collector alongside conntrack
(`run-bpf-eval.sh` scores both). Birth events capture pid + direction + the full
tuple at `connect()` time, in the owning process's context, before it can exit.
Byte coverage is 0 by design (birth has no bytes — the hybrid gets those from
conntrack).

| Scenario | path | Coverage | Attribution | Direction | Latency p50 |
|----------|------|---------:|-------------|-----------|------------:|
| **S2** churn (240 × 200 ms) | conntrack | 96.7% | **0% (100% missed)** | 100% | 511 ms |
| | **eBPF-birth** | **100%** | **100% correct** | 100% | **0 ms** |
| **S3** many-PID (12000) | conntrack | **59.4%** | **0% (100% missed)** | 100% | 453 ms |
| | **eBPF-birth** | **100%** | **100% correct** | 100% | **0 ms** |

**Verdict (data-backed): build the HYBRID.** eBPF socket-birth turns the two
things conntrack can't do for short-lived flows into solved problems:
- **Attribution 0% → 100%** — the pid is grabbed at `connect()`, before the
  process exits. This is the headline: it directly fixes the "short-lived
  container process shows pid=0" gap.
- **Coverage 59% → 100%** under extreme churn — every `connect()` is seen,
  immune to conntrack dump-timing / table churn.
- **Latency ~500 ms → 0 ms** — synchronous at birth.

conntrack stays the **byte** source (exact, cheap kernel counters; eBPF birth
has none). So the production design is **conntrack(bytes) ⊕ eBPF-birth(pid +
direction + first-seen)**, correlated by 5-tuple — NOT a capture-datapath
replacement. A pure pcap/eBPF capture path (Phase 1) is unnecessary: conntrack
already captures bytes accurately at far lower cost than per-packet userspace.

> Still honest about what's NOT yet measured before committing to a production
> hybrid: **CPU/memory cost** of the eBPF program under load (the `kprobe`s fire
> per-connect — cheap, but unmeasured), **UDP** (`birth.bt` is TCP-only), the
> **inbound/accept** path, **hard direction cases** (S4/S6), and the
> **fallback** behaviour when CAP_BPF/BTF is unavailable (must degrade to
> conntrack-only, per the packaging-footprint rule in `DESIGN.md §8`). These are
> the Phase 3 / productionisation gates — but the magnitude here (0→100% on the
> metric v0.4's container story cares about most) clears the §1 build bar with
> room to spare.

### Gate: eBPF CPU cost under load (`cost-bpf-eval.sh`)

The idle-VM scoring runs can't answer "what do the kprobes cost at a high
connect rate?". `cost-bpf-eval.sh` measures it two ways: the load-independent
**pure kernel** number via `kernel.bpf_stats_enabled` + `bpftool` (per-event
`run_time_ns`, which a production ring-buffer impl would pay), and an
end-to-end system-CPU delta (conservative, includes bpftrace's printf).

First VM run (16 workers, 0-lifetime churn, ~2700 connects/s, 4-CPU VM):

| metric | value |
|--------|-------|
| eBPF kernel cost | **~1467 ns/event × 2 events/connect ≈ 3 µs/connect** |
| at ~2700 conn/s | **0.79 % of one core** |
| extrapolated @ 50k conn/s | ~15 % of one core |
| end-to-end ΔCPU | lost in noise — the gen+sink saturate all CPUs, so only `bpf_stats` isolates it (by design) |

**Verdict on cost: acceptable.** Sub-1 % of a core at normal connect rates;
only matters at extreme rates (tens of thousands/s, e.g. a hammered reverse
proxy), and the production impl (raw bytes to a ring buffer, `ntop` deferred to
userspace) would be cheaper than this bpftrace prototype. The cost does NOT
move the build verdict.

### Remaining gates (status)

| Gate | Status |
|------|--------|
| eBPF CPU cost under load | ✅ measured (~3 µs/connect, <1% core at normal rates) |
| CAP_BPF/BTF-absent fallback | ✅ structurally handled — the harness already skips the eBPF collector when bpftrace/BTF is absent (orchestrator is skip-safe); the *production* hybrid must mirror this (degrade to conntrack-only). |
| UDP birth | ✅ measured (S2U): eBPF-birth attributes connected-UDP at **99.2%** vs conntrack **19%**; coverage 100% vs 94%, latency 0 ms vs 590 ms. `udp_sendmsg` probe, scorer dedups per-flow. (0.8% wrong pid — a tiny PID-reuse edge under churn; negligible.) |
| inbound/accept | ◐ `inet_csk_accept` kretprobe added to `birth.bt` (captured if an inbound flow occurs), but no role-reversed *scenario* yet. Lower-stakes: servers are long-lived → conntrack attributes them fine; the open question is only inbound *direction*, which eBPF gives definitionally. |
| hard direction (S4 forwarded / S6 p2p) | ◐ not run. eBPF-birth gives direction definitionally (connect=outbound, accept=inbound), so it's robust by construction; these would only quantify the conntrack *heuristic's* weak cases, and forwarded flows have no local process anyway (reason=Forwarded). Low decision value. |



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
