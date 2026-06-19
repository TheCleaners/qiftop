# qiftop design: BPF/pcap-vs-conntrack capture measurement harness

> **Status:** design only. This is a *measure-first* investigation. We do NOT
> build a libpcap/eBPF datapath until the numbers justify it, and we decide by
> **magnitude**, not vibes. The point of this document is to design the
> measurement rigorously enough that we don't repeat the methodological
> shortcuts we've taken before (see §6).

## 0. Why this exists

The Linux capture datapath today is **conntrack**: a periodic
`libnetfilter_conntrack` dump (1000 ms `QTimer`, separate `AF_INET` /
`AF_INET6` `NFCT_Q_DUMP`s), bounded to the top **4096** flows, requiring
`net.netfilter.nf_conntrack_acct=1` for byte/packet counters
(`src/backend/linux/ConntrackMonitor.cpp:324-334,362-400,413-457`;
`FlowTopK.h:31-54`). Direction is a **heuristic** — ephemeral-port + local-end
fallback, never SYN-observed, returns `Unknown` for ambiguous flows
(`src/util/ConnectionHeuristics.h:61-86`). Process/container attribution is a
**separate** layer (sock_diag + `/proc/<pid>/fd` + cgroup + netns) that is
**orthogonal to the capture method** — `ConntrackMonitor` just calls
`attributeFlows()` on whatever flow list it produced
(`src/backend/linux/ConntrackMonitor.cpp:463-479`).

The recurring question: would a **libpcap/BPF or eBPF** capture path be
materially better — more coverage, accurate direction, lower latency — and at
what CPU/memory cost, *under production-like load (many connections AND many
PIDs)*? There is already a **BSD pcap + SYN-direction** implementation to mine
as a reference (`src/backend/bsd/BsdConnectionWorker.cpp`).

## 1. Goals, non-goals, decision rule

### Goals
- Quantify, against **independent ground truth**, how conntrack and each
  candidate datapath compare on: **flow coverage, byte coverage, direction
  accuracy, attribution latency, capture latency, drop behaviour, and
  CPU/memory cost** — as a function of **offered load** (sweep, not a single
  point).
- Make the workload **production-shaped**: many concurrent flows AND many
  distinct (often short-lived) PIDs, with churn and PID reuse, IPv4+IPv6,
  TCP+UDP, plus forwarded/NAT and container/netns scenarios.
- Produce a **decision matrix** with magnitudes that maps directly onto a
  build/don't-build recommendation per candidate.

### Non-goals
- NOT building the production BPF/eBPF datapath. Prototype collectors here are
  throwaway measurement instruments, explicitly not production code.
- NOT a pipeline microbenchmark — `bench/` already covers aggregation, filter,
  top-K, DTO, TUI cost, and concluded *poll cadence, not pipeline, is the eager
  budget*. This harness measures the thing we never measured: **capture
  quality**.
- NOT a kernel-tuning study of conntrack itself (hashsize, timeouts) beyond
  recording the sysctls in effect.

### Decision rule (set thresholds BEFORE measuring)
Pre-committing thresholds stops us rationalising a result after the fact. Build
a new datapath only if it clears a **pre-registered** bar, e.g.:
- **Direction:** lifts correct-direction from the conntrack-heuristic baseline
  to **≥95%** with **<1%** wrong (not just fewer `Unknown`s), OR
- **Short-flow coverage:** recovers **≥X%** of ground-truth flows that conntrack
  misses (X to be fixed after the baseline run shows how many it misses), OR
- **Attribution latency:** cuts median flow-start→first-correct-attribution by
  **≥Z ms** at target load,
…**AND** the steady-state CPU/memory delta at target load stays within a
pre-set budget (e.g. <1 core of kernel+user CPU and <Y MB at 50k flows / 5k
PIDs). If conntrack is within a small margin on the metrics that matter, we
**keep it** — it's simpler, portable, privilege-light, and already shipped.
The thresholds above are placeholders; §8 fixes them numerically after Phase 0
establishes the baseline.

## 2. The question is NOT monolithic — decompose it

A single "BPF vs conntrack" verdict hides three independent sub-questions, each
with a different best answer. The harness must score them **separately** so we
don't let one strong axis mask a weak one (a classic gloss-over):

| Sub-question | What it measures | Candidate that targets it |
|---|---|---|
| **Q1 byte/flow coverage** | Does the path see every flow + its true bytes? | pcap (per-packet), eBPF skb accounting |
| **Q2 direction** | Outbound/inbound correct at flow birth? | pcap SYN observation, eBPF `tcp_connect`/`accept` |
| **Q3 attribution** | Right PID/container, fast, reuse-safe? | eBPF socket-lifecycle tracepoints |

Crucially, **Q3 attribution is reused identically across all capture paths**
(the sock_diag/proc/cgroup/netns chain is orthogonal —
`ConntrackMonitor.cpp:463-479`), so for Q1/Q2 it *cancels out* and we hold it
constant. The exception is the eBPF-socket-tracepoint candidate, which changes
attribution itself — measured as its own thing in Q3, and which suggests a
**hybrid** (conntrack/eBPF bytes + eBPF birth events for pid+direction) that may
be the real winner. The harness MUST include the hybrid as a first-class
candidate, not an afterthought.

## 3. Ground truth — the crux

Every prior "coverage"-flavoured claim we've made was unfalsifiable because we
had **no independent record of what actually happened**. This harness is built
around an authoritative ground-truth generator; everything else is scoring
against it.

### 3.1 The generator records what it DID, not what was captured
A controller spawns the workload and, for **every** flow, logs an authoritative
record (append-only, CLOCK_MONOTONIC timestamps), independent of any capture
path:

```
flow_id, proto(TCP/UDP), family(4/6),
local_ip, local_port, remote_ip, remote_port,
owner_pid, owner_starttime_jiffies,      # for reuse-safe matching
initiator(local=outbound / remote=inbound),  # the TRUE direction
bytes_local_to_remote, bytes_remote_to_local,
t_open_monotonic_ms, t_close_monotonic_ms
```

`initiator` is known because the generator decides which side calls
`connect()` vs `accept()` — that's the **definitional** direction, the thing the
heuristic only approximates. `owner_pid`+`starttime` is the definitional
attribution target.

### 3.2 Workload shape (parameterised scenarios)
A single "realistic" workload is a fiction; we **sweep** and report per
scenario. Each is a knob-set on one generator:

- **S1 steady-many-flow:** N long-lived flows (N ∈ {1k, 10k, 50k, 100k}),
  modest PID count. Tests table scale + top-K truncation
  (`kMaxInProcessFlows=4096` — conntrack will drop above it; does BPF?).
- **S2 churn-short-flow:** high flow *birth/death rate* with lifetimes
  **below** the 1 s poll (e.g. 50–500 ms), so conntrack's periodic dump
  structurally misses them. This is the scenario most likely to favour packet
  capture — and the one our steady-state benches never modelled.
- **S3 many-PID + reuse:** thousands of short-lived worker processes opening a
  few flows each, with `pid_max` lowered (or just high spawn rate) to force
  **PID reuse within the measurement window** — stresses the starttime guard
  and the attribution-latency metric.
- **S4 forwarded/router:** a `netns` + veth + a router namespace doing
  MASQUERADE, generating flows where **neither endpoint is host-local** — tests
  the forwarded path and direction `Unknown` behaviour.
- **S5 container/netns:** flows inside container netnses (reuse the Tier-2
  attribution runner containers) — tests cross-netns capture parity.
- **S6 production-blend:** a weighted mix of S1–S5 running concurrently, the
  closest thing to "production", reported but never used as the *only* number.

Each scenario runs at multiple **offered-load levels** so we can see where each
path's curve bends (the drop knee, the CPU knee) rather than reading a single
point and extrapolating.

### 3.3 Sink + traffic
A local echo/sink server (its own process, also logging accepted-connection
facts as a cross-check on the generator) on host + in netns/container.
TCP and UDP, v4 and v6. Known, fixed byte volumes per flow so byte-coverage is
exact-comparable. Deterministic seeds; fixed wall-clock duration per run;
**N repeats**, report **median + IQR**, not a single run.

## 4. Metrics, defined precisely

Vague metrics are how you accidentally cheat. Exact definitions:

- **Flow coverage** = |ground-truth flows seen in ≥1 snapshot| / |ground-truth
  flows|. Reported **split by lifetime bucket** (sub-poll vs multi-poll) and by
  proto/family — an aggregate number hides the S2 short-flow story.
- **Byte coverage** = Σ captured bytes / Σ ground-truth bytes, per-flow matched
  then aggregated. Conntrack should be ~exact on flows it sees (kernel
  counters) but 0 on missed ones; pcap may *under*-count under drops and
  *over*-count nothing. Report both **conditional** (on seen flows) and
  **unconditional** (of all offered bytes).
- **Direction accuracy** = per matched flow, captured vs ground-truth
  `initiator`. Report a 3×3 confusion matrix (Out/In/Unknown) — %correct,
  %wrong, %Unknown **separately** (turning `Unknown`→guess can raise "correct"
  while raising "wrong"; both matter).
- **Attribution accuracy** = captured (pid,starttime) vs ground truth:
  %correct, %missed (pid=0), %**wrong** (reuse/misattribution — the dangerous
  one).
- **Attribution latency** = first snapshot with the *correct* pid minus
  `t_open` (ground truth). Distribution, not mean.
- **Capture latency** = first appearance minus `t_open`.
- **Drops / loss, per path, explicitly** (never assume lossless):
  - pcap: `pcap_stats()` ps_drop/ps_ifdrop + snaplen truncation count.
  - eBPF map: map-full insert failures / eviction count (a BPF counter).
  - conntrack: table-full (`nf_conntrack_count` vs `_max`, `insert_failed` from
    `/proc/net/stat/nf_conntrack`) + our **top-K truncation** count
    (`ConntrackMonitor.cpp:449-457`).
- **Cost**, swept by load, measured **one path at a time** (observer effect):
  - userspace CPU per collector thread (`/proc/self/task/<tid>/stat`),
  - kernel CPU attributable: `perf stat` softirq/`ksoftirqd` + `bpftool prog`
    run_time_ns for eBPF, delta over an idle baseline,
  - RSS + the path's own kernel memory: conntrack table bytes, eBPF map bytes
    (`bpftool map`), pcap ring buffer size.
  - **Idle baseline subtraction:** also measure the cost of each mechanism
    *attached but no agent reading* (conntrack acct on vs off; eBPF prog loaded
    vs not) so we separate "kernel overhead of the mechanism" from "agent work".

## 5. Harness architecture

```
scenario orchestrator (per S1..S6, per load level, per repeat)
  ├─ setup: namespaces/veth/bridge, sysctl snapshot, kernel/BTF probe
  ├─ start sink server(s)            → sink ground-truth log
  ├─ start ONE collector under test  → snapshots.<path>.ndjson
  │     • conntrack  = the REAL ConntrackMonitor logic via a probe binary
  │     • pcap       = throwaway libpcap collector (mines BSD SYN logic)
  │     • ebpf-skb   = throwaway tc/cgroup-skb byte-accounting collector
  │     • ebpf-birth = throwaway tcp_connect/accept tracepoint collector
  │     • hybrid     = ebpf-birth (pid+dir) + conntrack/ebpf-skb (bytes)
  ├─ start generator                 → ground-truth.ndjson
  ├─ run fixed duration; sample cost (perf/bpftool/proc) on a side thread
  ├─ teardown + restore sysctls/namespaces
  └─ scorer: ground-truth ⋈ snapshots → metrics.<scenario>.<load>.json
```

Design rules that prevent gloss-over:
- **The conntrack collector is the production code path**, not a re-imagining.
  Wrap the real `ConntrackMonitor` (same 1 s cadence, same top-K, same acct
  handling) in a small probe — otherwise we'd measure a strawman. (The probe
  pattern already exists: `qiftop-attribution-probe`.)
- **Common snapshot schema** (flow key, rx/tx bytes, direction, pid+starttime,
  first/last seen ms) so the **scorer is path-agnostic** — one scorer, no
  per-path special-casing that could hide a bias.
- **One collector per run for cost numbers** (avoid mutual perturbation);
  coverage/accuracy MAY run paths back-to-back with identical generator seeds
  (deterministic replay) so they see the *same* workload without competing for
  CPU.
- **Record the environment**: kernel version, BTF presence, libnl/libpcap/
  libbpf versions, every relevant sysctl (`nf_conntrack_acct/max/buckets`,
  `rmem_max`), CPU model, whether in a VM. A result without its environment is
  noise.
- **Lives in `tests/perf/bpf-eval/` (or `bench/integration/`)**, gated behind a
  new opt-in (e.g. `QIFTOP_BUILD_BPF_EVAL=OFF`), **VM-only**, never in the pure
  `bench/` set or default `ctest`. Run on the Vagrant VM (controlled kernel),
  plus at least one **newer kernel** for the eBPF candidates.

## 6. Gaps we've glossed over before — and how this design closes each

This section is the reason for the "reasonable effort" ask. Each row is a real
shortcut from our prior process and the explicit mitigation here.

| Prior gloss-over | Mitigation in this design |
|---|---|
| Measured **pipeline cost** (aggregation/top-K/DTO) and *assumed capture was fine*; concluded "cadence is the budget" without checking what conntrack misses. | Ground-truth **coverage** is the headline metric (§3, §4). |
| Synthetic data was **steady-state** (`BenchData.h` long-lived flows). | **Churn/short-flow (S2)** and **PID-reuse (S3)** are first-class scenarios (§3.2). |
| **No ground truth** → "coverage"/"accuracy" claims were unfalsifiable. | Authoritative generator + sink logs; scorer joins against them (§3.1). |
| Implicitly assumed capture is **lossless**. | Explicit **drop accounting** on every path: pcap_stats, map-full, conntrack table-full + our top-K truncation (§4). |
| Cost measured **userspace-only**. | Kernel softirq/`ksoftirqd` via `perf`, eBPF `run_time_ns`, **idle-baseline subtraction**, plus table/map/ring **memory** (§4). |
| **Apples-to-oranges** comparisons (different inputs/host/kernel). | Same generated workload (deterministic replay), same host/kernel, recorded environment, one-path-at-a-time for cost (§5). |
| **Conflated** capture with attribution & direction. | Decomposed into Q1/Q2/Q3; attribution held constant for Q1/Q2 since it's orthogonal (§2). |
| **Single load point**, then extrapolate. | **Sweep** offered load; find the knee per path (§3.2, §4). |
| Ignored the existing **BSD pcap+SYN** precedent and re-derived from scratch. | pcap candidate **mines `BsdConnectionWorker`** SYN logic (§5, §7). |
| **IPv6/UDP under-tested** vs IPv4/TCP. | Workload mixes both; metrics reported **per family/proto** (§4). |
| "**Works on my kernel**" / dev-box-only. | VM-only + a second newer kernel; BTF/sysctl/version recorded; eBPF gated on capability probe (§5). |
| Reported a **single run** as truth. | **N repeats, median + IQR** (§3.3). |
| Let a strong axis (e.g. "pcap sees more packets") imply a **monolithic win**. | Separate per-sub-question scoring + a per-metric decision matrix (§2, §8). |

## 7. Candidate-specific measurement notes

- **pcap (Q1+Q2):** BPF filter to TCP/UDP headers only (small `snaplen`),
  observe SYN / SYN-ACK for TCP direction and first-packet for UDP. Measure
  per-packet userspace aggregation cost, copy overhead, and **ring size vs drop
  rate** under pps sweep. Reuse `BsdConnectionWorker` SYN-direction logic. Byte
  totals come from summing observed payload — note this *diverges* from
  conntrack's kernel counters on drops; that divergence IS a metric.
- **eBPF skb accounting (Q1):** tc-egress/ingress or `cgroup/skb` program adds
  bytes into a per-flow `BPF_MAP_TYPE_HASH` keyed by the 5-tuple; userspace
  iterates the map each tick. Measure map-iterate cost, map memory, map-full
  eviction policy + count. Needs `CAP_BPF`/`CAP_SYS_ADMIN`, BTF/CO-RE — record
  kernel support; SKIP cleanly where unsupported.
- **eBPF socket-lifecycle (Q2+Q3):** tracepoints/kprobes on `tcp_connect`
  (outbound, exact pid), `inet_csk_accept` (inbound, exact pid),
  `udp_sendmsg`/`udp_recvmsg` (first-packet direction). Gives **exact
  pid+direction at flow birth** — eliminates both the direction heuristic and
  the PID-reuse race. Bytes still need conntrack or skb accounting → motivates:
- **Hybrid (the dark-horse):** eBPF-birth for pid+direction ⊕ conntrack
  (already shipped, kernel-counted bytes) for volume. Likely the best
  accuracy/cost trade because it keeps conntrack's cheap, exact byte counters
  and only adds lightweight birth events. **Must be measured**, because if it
  wins we get most of the benefit without a full packet-capture datapath.

## 8. Decision matrix (filled after measurement)

Per candidate × per scenario × per load, a row of: flow-cov%, byte-cov%,
dir-correct% / dir-wrong%, attr-correct% / attr-wrong%, attr-latency p50/p95,
drop%, ΔCPU (user+kernel) vs baseline, Δmem. Then a **verdict per candidate**
against the §1 pre-registered thresholds: `keep-conntrack` /
`build-pcap` / `build-ebpf-skb` / `build-ebpf-birth` / `build-hybrid`. The
recommendation is whichever clears its bar at the **lowest** cost; ties go to
the **simpler/more-portable/less-privileged** option (conntrack < pcap <
eBPF).

### Packaging-footprint constraint (hard rule)
Whatever wins, the **default install footprint stays minimal**: conntrack
(libnetfilter_conntrack, already a hard dep) remains the datapath the agent
needs in its default config. If a pcap/eBPF path ships, its libraries
(**libpcap**, **libbpf**/BTF tooling) are **optional** deps —
`Recommends`/`Suggests` on Debian, weak deps on RPM, NOT `Depends`/`Requires` —
unless we make a deliberate, documented decision to switch the *default*
datapath. The agent must still come up and capture with the new libs absent
(falling back to conntrack). The measurement harness itself is **never
packaged** — opt-in (`QIFTOP_BUILD_BPF_EVAL`), VM-only tooling — so its
libpcap/libbpf build-deps add **zero** runtime package weight.

## 9. Phasing & deliverables

- **Phase 0 — scaffolding + baseline (highest value, lowest risk).** Generator
  + ground-truth/sink logs + scorer + the **conntrack collector** (real code)
  + the S1/S2/S3 scenarios + environment capture. Deliverable: the
  **conntrack baseline** — *how many short flows does it actually miss, how
  often is direction wrong/Unknown, what's its attribution latency*. This alone
  is worth doing: it numerically fixes the §1 thresholds and may show conntrack
  is good enough (or clearly not).
- **Phase 1 — pcap collector** + S4/S5; measure Q1/Q2.
- **Phase 2 — eBPF skb + eBPF birth collectors** (newer kernel); measure
  Q1/Q2/Q3.
- **Phase 3 — hybrid + the decision report** with the filled §8 matrix and a
  recommendation.

Gate stop-points: if Phase 0 shows conntrack already meets the bar for our
real workloads, we **stop and keep conntrack** — measure-first means we're
allowed to not build anything.

## 10. Risks & honesty caveats

- eBPF prototypes are kernel-version-sensitive (CO-RE/libbpf/BTF). Budget for
  it; SKIP-not-fail on unsupported kernels; never let an eBPF gap silently
  become "eBPF looked worse".
- A generator cannot *be* production. We mitigate by sweeping + scenario
  decomposition, and we **report it as scenarios, not as "the" answer**.
- Kernel-CPU attribution is noisy → `perf stat`, idle-baseline subtraction,
  N repeats, median+IQR; pin affinity / disable turbo on the VM where possible.
- **Observer effect:** cost runs are one-path-at-a-time; coverage runs use
  deterministic replay so paths see identical work without competing.
- The honest null result ("conntrack is fine") is a **success**, not a failure
  — the whole point of measure-first.

## 11. Open questions for @ebenali (steer before Phase 0)

1. **Thresholds (§1):** are the placeholder bars (dir ≥95%/<1% wrong; CPU <1
   core & <Y MB at 50k flows/5k PIDs) the right shape, or do you have a
   different "worth it" line in mind?
2. **Where it lives:** `tests/perf/bpf-eval/` vs `bench/integration/` — and is
   VM-only (Vagrant) acceptable as the canonical venue, or do you want a
   second bare-metal box?
3. **Scope of Phase 0:** start with just S1/S2/S3 (host-only, no eBPF) to get
   the conntrack baseline fast, then decide whether Phases 1–3 are even worth
   it? (Recommended.)
4. **eBPF appetite:** are we willing to take a `CAP_BPF` + BTF dependency in
   production *if* the hybrid wins, or is "no new kernel-version-sensitive
   dependency" a hard constraint that should pre-empt the eBPF candidates?
