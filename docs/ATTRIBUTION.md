# Process & container attribution — the field guide

This document captures the **why** and **wisdom** behind qiftop's
per-flow attribution pipeline. The implementation is in
`src/backend/linux/`; this is the lore that explains why it looks the
way it does, what we tried first, and the cliffs we found at the
edges of the documentation.

It is written for two audiences:

* New contributors to qiftop who need to extend the attribution code
  without re-learning the bear traps.
* Anyone building a tool that needs to answer "which process/container
  on this box is responsible for this TCP/UDP flow?" on Linux — most
  of what follows is generally applicable, not qiftop-specific.

---

## 1. The problem in one paragraph

iftop tells you *which 5-tuples* are using bandwidth. The interesting
question for a sysadmin is **which process**, or — increasingly —
**which container** is responsible. Linux has no single API that
answers this; you must compose three or four orthogonal data sources,
defend the joins against multiple races, and handle the fact that the
flow's true holder may live in a different network namespace from your
observer. None of these data sources are documented with attribution
as their primary use case, so the right paths and corner cases are
folklore. This doc is our folklore.

---

## 2. The shape of the answer

For every observed flow we want to emit:

| Field              | Source                                            |
|--------------------|---------------------------------------------------|
| `pid`              | sock_diag inode → `/proc/<pid>/fd/*` reverse-walk |
| `comm` / `uid`     | `/proc/<pid>/status`                              |
| `cmdline` / `exe`  | `/proc/<pid>/{cmdline,exe}`                       |
| `container.runtime`| classify `/proc/<pid>/cgroup` line                |
| `container.id`     | short hex ID, human name, or `unit:<name>` from the cgroup path |
| `container.name`   | reserved; currently empty (no CLI/API lookup in the hot path) |

Three properties matter:

1. **Best-effort, not authoritative.** A flow may legitimately have
   no resolvable pid (kernel sockets, just-died process, foreign netns
   we can't enter). Every field is `optional`.
2. **Stale-by-construction.** PIDs are reused. We snapshot the
   process's `starttime` jiffies at enrollment and re-check at lookup;
   a mismatch means the cache is poisoned and we drop the answer
   rather than misattribute (`ProcSnapshot::pidStartTime`,
   plus the resolver starttime guards).
3. **Cheap.** sock_diag dumps are the cheapest API the kernel exposes
   for "list every socket"; we rebuild caches at most once per agent
   tick (~1 Hz).

---

## 3. Pipeline overview

```
                                  ┌─────────────────────────────┐
   conntrack flow      ─────▶     │ SockDiagResolver            │ ─── pid
   (4-tuple + iface)              │   sock_diag NETLINK_SOCK_DIAG│
                                  │   + /proc/<pid>/fd reverse  │
                                  └──────────────┬──────────────┘
                                                 │ pid
                                                 ▼
                                  ┌─────────────────────────────┐
                                  │ CgroupClassifier            │ ─── container
                                  │   /proc/<pid>/cgroup        │     {runtime,id}
                                  │   + regex table             │
                                  └─────────────────────────────┘

   (in parallel, for flows held in foreign netnses)

                                  ┌─────────────────────────────┐
   conntrack flow      ─────▶     │ NetnsScanner                │ ─── pid (in
   (4-tuple + iface)              │   worker thread cycles      │     container netns)
                                  │   through every netns,      │
                                  │   sock_diag per ns, builds  │
                                  │   inode → pid map.          │
                                  └─────────────────────────────┘

   All resolvers are siblings under CompositeResolver, which returns
   the FIRST non-nullopt answer per query.
```

The split into independent resolvers behind a `CompositeResolver` is
deliberate: each can be unit-tested in isolation, each declares its
own `capabilities()`, and they fail independently (no `CAP_SYS_ADMIN`
just makes `NetnsScanner` skip namespaces it cannot enter — the rest
keep working).

The factory order is deliberate and currently fixed:
`SockDiagResolver` → `CgroupClassifier` → `NetnsScanner`, gated by the
compile-time `QIFTOP_ENABLE_*` switches (default ON on Linux) and each
resolver's startup probe. `CompositeResolver` asks children in that
order and returns the first useful answer for each method; e.g.
`resolvePid()` uses host sock_diag first, then cross-netns results,
while `resolveContainerForPid()` is supplied by the cgroup classifier.

Capability tokens come from the resolvers whose startup probes
succeeded:

| Resolver | Tokens |
|----------|--------|
| `SockDiagResolver` | `process-attribution` |
| `CgroupClassifier` | `container-attribution`, `container-chain` |
| `NetnsScanner` | `netns-scan` |

`InterfacesService` also mirrors these into wire-level UI gates:
`process-attribution-wire`, `container-attribution-wire`, and
`container-chain-wire` (the last requires both `container-attribution`
and `container-chain`). `netns-scan` means the worker started; individual
namespace entries can still be skipped later if `setns()` is denied.

---

## 4. Data source #1 — `NETLINK_SOCK_DIAG`

This is the modern replacement for parsing `/proc/net/tcp`. It is:

* **Atomic-ish.** A single dump returns a consistent snapshot at the
  netlink layer's reader-writer lock; `/proc/net/tcp` walks the
  socket hash table without one.
* **Cheap.** Milliseconds even with tens of thousands of sockets.
* **Privileged for cross-user lookups.** Unprivileged callers get
  their own sockets; full visibility needs `CAP_NET_ADMIN` (or root).

What qiftop uses from each socket: the 4-tuple and, crucially, the
**kernel inode of the socket** (`idiag_inode`). The inode is the join
key for the next step. sock_diag also exposes an inet UID, but qiftop
currently reads UID next to `comm` from `/proc/<pid>/status` after the
inode→pid join.

What it does **not** return: the owning PID. Linux does not maintain
socket→pid in-kernel because sockets are fds (multiple processes can
own the same fd via `dup`/fork/SCM_RIGHTS).

### Reverse-mapping socket inode → pid

We walk `/proc/<pid>/fd/*`, `readlink`-ing each one. Targets that
match `socket:[<inode>]` give us a row in a `inode → pid` map.

Lessons:

* `QDir::Files` filters by `S_ISREG` of the symlink target, which an
  unresolved `socket:[…]` link is not. Use `QDir::System` instead
  (or the current raw `readdir`/`readlink` walk).
* If a PID dies between starttime read and fd walk, just drop it;
  don't propagate the error.
* **Snapshot the starttime at enrollment**, store it next to the pid,
  re-read at lookup time. If they differ, the kernel reused the pid
  for a fresh process and your cache is lying — return `nullopt`,
  don't return the wrong cmdline.

### Socket-key matching: 4-tuple first, local 2-tuple fallback

The socket dump records each socket under two lookup keys:

1. the full key: `(proto, local-address, local-port,
   remote-address, remote-port)`;
2. a tagged local-only key: `(proto, local-address, local-port)`.

`SockDiagResolver::resolvePid()` then tries, in order:

1. exact full 4-tuple — connected UDP and established TCP;
2. exact local 2-tuple — unconnected UDP sockets and TCP listeners
   bound to one address;
3. wildcard local 2-tuple — sockets bound to `0.0.0.0:<port>` or
   `::<port>`.

This matters because common server sockets do **not** carry the remote
peer in sock_diag. An unconnected UDP socket (the usual `recvfrom()`
service shape) and a TCP listener both report `idiag_dst` as
`0.0.0.0:0` / `:::0`, while conntrack rows contain the real peer. A
4-tuple-only join therefore missed those flows entirely. Indexing the
local-only key and falling back through exact-local then wildcard-local
is what made UDP services and listener-side TCP attribution work; on a
busy host this raised overall flow attribution from roughly 17% to 77%.

The full key is still tried first so connected sockets keep their
precise peer identity. The local-only key lives in a distinct tagged
namespace (`sockdiag::makeLocalKey`) so it cannot collide with a full
4-tuple cache entry.

---

## 5. Data source #2 — `/proc/<pid>/cgroup`

Once you have a PID, the cgroup path tells you the container scope.
The file format trips up almost every implementation:

```
# v2 (modern, unified hierarchy):
0::/system.slice/docker-3e76d6...........8a.scope

# v1 (legacy, one line per controller):
12:memory:/docker/3e76d6...........8a
11:cpu,cpuacct:/docker/3e76d6...........8a
...
```

`extractPath()` handles both: prefer the v2 line
(`0::/path`); fall back to any v1 line if v2 isn't present. **Every
modern Linux has v2**; the v1 fallback exists for old RHEL 7-era
hosts where qiftop might be installed for portability.

### The runtime regex cookbook

We classify the path against a table of regexes (`classifyPath` /
`classifyPathChain`). The patterns matter more than they look — each
one was discovered the hard way:

| Runtime    | Pattern (real example)                                                | Notes |
|------------|------------------------------------------------------------------------|-------|
| docker     | `/system.slice/docker-<64hex>.scope` <br> `/docker/<64hex>`            | Both shapes exist; systemd cgroup driver vs cgroupfs driver. |
| containerd | `cri-containerd-<64hex>.scope` <br> `.../pod<uid>/<64hex>`             | k8s-on-containerd. The bare 64-hex cgroupfs leaf only counts when it immediately follows a kubernetes pod segment. |
| cri-o      | `crio-<64hex>.scope` <br> `.../pod<uid>/<64hex>` when CRI-O is probed  | Systemd-driver paths identify CRI-O directly. Cgroupfs-driver CRI-O is indistinguishable from containerd, so `CgroupClassifier::initialize()` flips a hint when `/run/crio/crio.sock` exists. |
| kubernetes | `kubepods[./].*?pod<32-72hex_underscores>` <br> bare `pod<uid>`        | Pod-level fallback when no runtime-specific leaf is present. Underscores in the class because systemd-escaped UIDs are `665b0949_7b83_…`, NOT dashes. |
| podman     | `libpod-<64hex>.scope` <br> `libpod-<64hex>`                           | Systemd and cgroupfs shapes; the no-`.scope` branch must be checked before the generic kubepods match. |
| lxd        | `lxd-<name>.service`                                                   | Match the `.service` segment; tested before generic systemd unit so LXD wins. |
| lxc        | `/lxc(\.payload)?[./]<name>`                                           | Plain LXC, names not hex. |
| nspawn     | `/machine.slice/machine-<NAME>.scope` <br> `systemd-nspawn@<NAME>.service` | systemd-nspawn registered via machined (default) or started via the template unit. NAME is human-readable, NOT a content-addressable hash — distinguishes nspawn from every other supported runtime. Caveat: libvirt VMs (when libvirtd has machined integration) also register as `machine-qemu\x2d<id>.scope` and get mislabelled `nspawn`; the machine name typically makes it obvious. |
| systemd    | `/<unit>.{service,socket,mount}`                                       | Non-container scope but useful — UI labels it `unit:nginx.service`. Excluded under `/user.slice` so desktop sessions stay host processes. |

Order matters in `classifyPath`: lxd before generic systemd, kubepods
fallback after the runtime-specific scopes, etc.

**The 12-char short ID** is what every hex-ID CLI shows (`docker ps`,
`podman ps`, `crictl ps`); hex-ID runtimes slice with `id.left(12)` to
match what a sysadmin pastes from elsewhere. Human-name runtimes
(`lxc`, `lxd`, `nspawn`) keep the full captured name.

### Host-vs-container heuristic

These cgroup paths mean **the host**, not a container:

* empty
* `/`
* `/init.scope`
* anything under `/user.slice` (desktop user managers and apps)

`classifyPath` returns `nullopt` for them. If no container-shaped
segment matches, a system-level unit under e.g. `/system.slice` may get
the fallback label `systemd:unit:foo.service`; otherwise the process
stays a plain host process.

### 5a. Nested containers — leaf wins, chain is preserved

What we learned bringing up the **k3d** (k3s-in-docker) integration
test: a single cgroup path can encode several container scopes
*at once*, and matching "first regex anywhere" returns the wrong one.

A real k3d pod's `/proc/<pid>/cgroup` reads (one line, wrapped for
legibility):

```
0::/system.slice/docker-503bd...scope                 ← k3d node container
   /kubepods.slice/kubepods-besteffort.slice
   /kubepods-besteffort-pod665b...slice               ← pod slice
   /cri-containerd-cd32fa6a7e74...scope               ← the actual workload
```

Two rules fall out of this:

1. **Classify per segment, leaf wins.** The classifier splits the path
   on `/` and walks the segments. For "single answer" callers
   (`classifyPath` / `resolveContainerForPid`) the **innermost**
   classified segment is returned — that's the actual container the
   process is running in. The k3d node wrapper is interesting context,
   but `nc` inside the pod is owned by `containerd:cd32fa6a7e74`, not
   `docker:503bd477c49a`.
2. **Keep the full chain available.** `classifyPathChain` returns the
   whole OUTER→INNER list so consumers that *do* care about nesting
   (ops dashboards, "show me which k3d node this pod is on") get it
   for free. The resolver interface exposes the same via
   `ProcessResolver::resolveContainerChainForPid`; capability token
   `container-chain` advertises that the resolver actually populates
   more than one entry. The chain is depth-capped at
   `kMaxContainerChainDepth` (16) with a `qWarning` on truncation —
   real paths don't get within sight of this even in deeply-nested
   k8s-on-podman-on-LXD setups.

Practical edge case: the **systemd-unit fallback** (e.g.
`unit:nginx.service`) is only emitted when nothing container-shaped
matched anywhere in the path. A pod whose pause container lives under
a `*.service` slice shouldn't be relabelled `systemd` just because
some ancestor segment happens to look like a unit — the
`kubepods`/`containerd` segments are more informative.

### 5b. Naked k8s vs. k3d — chain shape as a regression assertion

The chain shape is the cheapest *distinguishing* signal between
"real" k8s and k8s-in-docker, and the test suite exercises both:

| Flavour                  | Tier-1 coverage                                              | Tier-2 runner       | Expected chain (outer → inner) |
|--------------------------|--------------------------------------------------------------|---------------------|-------------------------------|
| Docker plain             | `docker_v2_cgroupfs.txt` (fixture)                           | `run-docker.sh`     | `[docker]`                    |
| k3d (k8s-in-docker)      | `test_cgroup_parse.cpp::k8sCgroupfsDriverK3dShape` + `k3sPodInDockerPrefersInnermost` (inline synthetic — fixture pending an upstream-sourced sample) | `run-k3d.sh`        | `[docker, kubernetes, containerd]` (depth 3) |
| Naked k8s, cgroupfs      | `test_cgroup_parse.cpp::k8sNakedCgroupfsDriver` (inline synthetic — fixture pending an upstream-sourced sample) | `run-k8s.sh` (k0s)  | `[kubernetes, containerd]` (depth 2, **no `docker`**) |
| Naked k8s, systemd       | `test_cgroup_parse.cpp::k8sNakedSystemdDriver` (inline synthetic — fixture pending an upstream-sourced sample) | `run-k8s.sh` (k0s)  | `[kubernetes, containerd]` (depth 2, **no `docker`**) |

Three of the four rows above are currently pinned by **inline
synthetic paths** in `tests/test_cgroup_parse.cpp` rather than by
fixture files in `tests/fixtures/cgroup_real/`. Per §6.3a's policy
("each runtime + driver combination MUST have at least one fixture
sourced from authoritative upstream documentation"), they should
eventually grow real fixtures harvested from k3s.io / kubernetes.io
issue trackers — the inline tests verify our regexes, not the
upstream cgroup path shape. The chain assertions are still real:
they exercise `classifyPathChain` on representative paths, and a
regression in the regexes WOULD fail them.

The Tier-2 `run-k8s.sh` includes a `grep -q '"runtime":"docker"'`
post-check on the probe's JSON output that MUST fail — catching any
future change to `classifyPathChain` that hallucinates a phantom
docker wrapper around naked containerd pods. The k0s runner uses
`k0s install controller --single`, started lazily by the runner so
`vagrant up` stays fast for developers only iterating on the docker
runner. (See AGENTS.md §6.5a for the cross-runner ordering gotcha
that this k0s install creates on shared VMs.)

### 5c. Hex-ID runtimes vs. name-ID runtimes

Not all "container IDs" are the same shape. Two distinct families:

* **Content-addressable / hex** — docker, podman, containerd-via-k8s,
  cri-o. ID is a sha256-derived 64-hex string in the cgroup path;
  CLI tools (`docker ps`, `podman ps`, `crictl ps`) show the first
  12 chars. CgroupParse calls `m.captured(1).left(12)` so the value
  we ship matches what a sysadmin would paste.
* **Human-name** — lxd, lxc, systemd-nspawn. ID is a name the user
  chose (`alpine`, `myguest`, `debian`). CgroupParse captures the
  full name; **never apply `.left(12)`** — it would truncate
  meaningful identifiers (`a-long-machine-na...` is worse than
  useless).

When adding a new runtime classifier, decide which family it belongs
to BEFORE writing the regex. The wire DTO (`ConnectionDto.containerId`)
is untyped string for both; the cosmetic-vs-truncate decision lives
entirely in CgroupParse. Downstream code makes no length/hex
assumptions (verified via grep `id.left` / `id.length`).

### 5d. Chain-shape "MUST NOT contain X" as a regression assertion

The Tier-2 runner suite exploits a useful property of nested-runtime
chains: each runtime combination has a CHARACTERISTIC SHAPE that
differs from neighbouring combinations:

|                        | Depth | Has `docker`? | Has `kubernetes`? |
|------------------------|-------|---------------|-------------------|
| Plain docker           | 1     | yes           | no                |
| Plain podman           | 1     | no            | no                |
| Naked k8s (k0s)        | 2     | **no**        | yes               |
| k3s-in-docker (k3d)    | 3     | **yes**       | yes               |

The "MUST NOT" rows are as informative as the "MUST" rows. If
`classifyPathChain` ever starts hallucinating a `docker` wrapper
around naked containerd pods (e.g. because some future regex grows
a false-positive match on `kubepods.slice`), the unit-test chain
length check will go from 2 → 3 and the `grep -q '"runtime":"docker"'`
in `run-k8s.sh` will succeed when it must fail. Cheap regression
trap; reuse the pattern when adding any new nested-runtime
classifier.

### 5e. Why we stopped adding Tier-2 runners after the fourth

Empirical observation from bringing up docker → podman → k3d → k8s
runners: each new Tier-2 runtime is a steeply diminishing-returns
investment.

* The **regex-drift risk** is what Tier-1 fixtures protect against,
  and a Tier-1 fixture takes ~10 minutes (drop a path-shape under
  `tests/fixtures/cgroup_real/`, add one row to the data table).
* The **NetnsScanner-walks-into-the-container-netns** code path is
  generic across every runtime that uses Linux network namespaces
  (i.e. all of them); docker already exercises it end-to-end. Adding
  a 5th runner mostly re-proves the same setns(2) plumbing.
* Each new Tier-2 runner adds **at least one bridge** to the test
  VM (`docker0`, `cni-podman0`, `kube-bridge`, ...) and the
  cross-runner state pollution risk grows superlinearly (k0s'
  kube-router rules break podman; see AGENTS.md §6.5a).

So our rule: **Tier-1 is mandatory, Tier-2 is opportunistic.** Add
Tier-2 only when a runtime has a genuinely-novel cgroup or netns
shape (k3d's nested chain qualified; cri-o didn't; nspawn might
have if not for `id.left(12)` being already correct). cri-o and
nspawn live happily at Tier-1.

---

## 6. Data source #3 — cross-namespace netns scanning

The bear trap that broke our initial design. **Conntrack lives in the
network namespace where the connection was tracked, and the agent
only sees host-netns conntrack** unless you do something explicit.
That's enough for the common case (rootful docker + bridge + NAT —
the host conntracks the masqueraded flow), but it falls apart for:

* macvlan/IPv6 (no NAT, packet path skips host conntrack table)
* `--net=container:other` (shared netns)
* anything bridged but not NAT'd

For these, the flow's *holder socket* lives inside a container's
netns. The kernel inode we got from the host-side sock_diag dump
doesn't exist there. We need to **enter every netns and dump
sock_diag inside it**.

### setns(2) and the threading rule

`setns(fd, CLONE_NEWNET)` changes the **calling thread's** network
namespace. This has consequences:

* **You must do all netns dancing on a dedicated worker thread.**
  Sharing the worker with anything else means that "anything else"
  randomly executes inside whatever namespace the last setns call
  left it in. Future netlink dumps, file reads, anything network-
  related becomes nondeterministic. `NetnsScanner` owns its
  `NetnsScannerWorker` and never shares the thread.
* **Always restore the anchor netns.** Wrap every target setns in a
  RAII guard that setns()es back on scope exit. If the restore
  fails, `qFatal` — running the next dump in a stranger's namespace
  would silently mis-attribute every subsequent flow.
* **Netlink sockets are bound to the netns they were created in.**
  You cannot reuse the host's sock_diag socket inside a container
  netns; you must open a fresh `NETLINK_SOCK_DIAG` socket *after*
  setns'ing into the target. Same for `AF_INET` `socket(2)` fds —
  they remember their birth netns forever.

### Enumerating netnses

* Walk `/proc/<pid>/ns/net` for every pid. The symlink target encodes
  the inode: `net:[4026532543]`. Dedupe by inode.
* Skip the inode of `/proc/1/ns/net` — that's the host netns, already
  covered by the main dump.
* Open the symlink as an fd; that's what you pass to `setns(2)`.
  `fstat()` it before entering and verify the inode still matches the
  one you enumerated, or a recycled representative pid could send you
  into the wrong namespace.
* The worker refreshes every 5 s and caps each pass at 256 non-host
  namespaces. Socket maps are capped at `sockdiag::kMaxSocketEntries`
  (65,536) so a pathological host cannot OOM the agent.

### Failure modes (all routine, all silent)

| Error                           | Cause                                       | Action |
|---------------------------------|---------------------------------------------|--------|
| `ENOENT` reading `/proc/X/ns/net` | pid vanished between readdir and open    | skip   |
| `EPERM` on setns                | missing `CAP_SYS_ADMIN` or systemd namespace sandbox | skip that namespace (capability may still be advertised) |
| `EINVAL`/`ENOENT` on setns/open | netns destroyed mid-walk                    | skip   |
| sock_diag dump fails post-setns | netns has no inet (unusual)                 | log once per cycle, continue |
| setns(anchor) restore fails     | catastrophic — anchor netns gone            | `qFatal`, agent dies cleanly |

The rule is: **anything except restore failure is routine**. Don't
spam logs.

---

## 7. Race conditions — the four we actually defend against

Attribution is *all* races. The interesting ones, and how we handle
each:

### 7.1 PID reuse

The kernel's pid space is small (32k by default, 4M with
`/proc/sys/kernel/pid_max` bumped). A long-lived agent will see pid
recycling within minutes on a busy host.

**Defence:** snapshot `/proc/<pid>/stat` field 22 (starttime in
jiffies since boot) at enrollment, store it next to the pid in every
pid-bearing cache (`SockDiagResolver` host maps, `NetnsScanner` maps,
`CgroupClassifier` per-pid cache, and the agent's per-snapshot
memoisation). At lookup, re-read and compare; mismatch → drop the
cached entry, return `nullopt`. The starttime is monotonic per real
process; a new process reusing the pid will have a higher value.

Cost: one `read` of a small file per cache miss. Negligible.

### 7.2 Process exit between cgroup read and use

We may classify a process's cgroup, then by the time the UI renders
the row, the process is gone. We never re-resolve from a cached
container label — the container info is captured at enrollment and
stored alongside the pid stamp. If the pid is reused, both the pid
*and* its container label are dropped together.

### 7.3 Cgroup deletion mid-classify

When a container exits, its cgroup directory is removed
asynchronously. `/proc/<pid>/cgroup` may give us a path that no
longer exists by the time we'd `stat()` it — which is why
**`CgroupClassifier` never touches the filesystem after reading
`/proc/<pid>/cgroup`**. The path is just a string; we regex it
in-memory.

### 7.4 Netns destruction mid-scan

A container exits between our `readdir` of `/proc/*/ns/net` and our
`setns` into its namespace fd. `setns` returns `EINVAL` (or `ENOENT`
on the open). Both are silent skips. The next scan cycle picks up
the new reality.

### 7.5 Bonus: conntrack mid-table eviction

Conntrack entries time out and disappear; a flow we just observed may
not be in the table 10s later when we try to re-resolve. We don't
hold conntrack references across ticks — every tick gets a fresh
dump.

---

## 8. The rootful vs rootless container trap

This bit us hard during the podman runner work and is worth
internalising before adding any new container runtime to the test
matrix.

**Rootless container runtimes** (rootless docker, rootless podman,
rootless containerd) commonly use **user-mode networking proxies** —
`slirp4netns` or `pasta`. These run as ordinary processes on the host
and pump packets to/from the container's netns over a tun device
inside the container. The flow has TWO endpoints from the kernel's
perspective:

* Inside the container netns: a TCP socket owned by the actual
  workload process.
* On the host side: a TCP socket owned by `pasta(1)` or
  `slirp4netns(1)`, the userspace proxy.

When the host's conntrack sees the flow, the *holder process* on the
host is `pasta`, not the container. Our attribution chain will
faithfully report `pid=<pasta>, runtime=<none>, container=<none>` —
which is *correct* but completely useless to the user.

**Rootful container runtimes** use a real Linux bridge (`netavark`
for podman, `docker0` for docker) and conntrack on the host sees the
masqueraded flow. The container's socket lives in the container's
netns, and `NetnsScanner` finds it. Attribution works.

**Implication for the integration test harness:** rootful only. The
`run-podman.sh` runner forces `sudo podman`. The Vagrant VM ships
docker.io + podman; both are exercised rootful via `ctest` (which
runs as root inside the guest).

**Implication for users:** if you run qiftop against a host that uses
rootless containers, you'll get the proxy process labelled. We could
in principle special-case `pasta`/`slirp4netns` to "look through" the
proxy by inspecting their netns peer — that's future work.

---

## 9. The probe binary: testability over elegance

The attribution stack is hard to unit-test directly because it needs
real PIDs, real cgroups, real netnses. We could mock everything; we
chose instead to ship a tiny CLI binary (`qiftop-attribution-probe`)
that takes a 4-tuple on the command line and prints JSON like:

```json
{
  "capabilities": ["process-attribution", "container-attribution", "netns-scan"],
  "container": {"id": "c605944b39cf", "name": "", "runtime": "docker"},
  "process":   {"pid": 5180, "comm": "nc", "uid": 0, ...},
  "status": "ok"
}
```

The probe wires up the same `ProcessResolverFactory` chain the agent
uses. The integration tests (`tests/integration/attribution/`) build
the probe, spin up a container, observe the resulting flow with
`ss -tnH`, then invoke the probe and assert on the JSON.

This is **end-to-end with the real resolver chain** — no fakes, no
mocks. The probe is 100% production code; the test harness just
drives it.

Worth doing again on any other piece of "compose many syscalls"
logic: the cost of a thin CLI is paid once and bought us:

* The ability to swap runtimes (docker, podman, k3d, cri-o) without
  rewriting any C++.
* The ability to debug "why didn't attribution work?" on a real
  cluster — just run the probe by hand.
* A natural integration-test seam that doesn't fight the agent's
  threading model.

---

## 10. Test harness lessons (what broke in CI/devbox)

### 10.1 The flow must be long enough to observe

Our first runner used `nc -w 1`, blinked the container, and then
asked the probe. By probe time the conntrack entry had already
moved to `TIME_WAIT` and the container's socket was closed. We
switched to `nc -w 30 ... </dev/zero` — feed it junk for 30 seconds
so the socket stays open across both the `ss` observation and the
probe attribution.

### 10.2 NFS-default vagrant mount

`vagrant up` on libvirt auto-adds a `/vagrant` synced folder backed
by NFS, which needs the host to run `nfsd`. We don't, so the mount
fails and `vagrant reload` errors out. Fix: explicitly
`config.vm.synced_folder ".", "/vagrant", disabled: true` and use
only the named `rsync` share.

### 10.3 Polkit + libvirt group

`qemu:///system` triggers a polkit prompt unless the caller is in
the `libvirt` group. There is a rootless alternative (`qemu:///session`)
but it forces `qemu-bridge-helper` which requires setuid + an ACL
edit on `/etc/qemu/bridge.conf` — more invasive than the standard
group setup. We default to system + libvirt group, and the
`scripts/local-integration.sh` wrapper fails fast with a one-line
fix-up hint if the user isn't a member yet.

### 10.4 Sudoers timestamp_timeout

Devboxes commonly have `Defaults timestamp_timeout=5` (or even 0)
in `/etc/sudoers`. A 5-minute integration run will re-prompt for
password mid-test and silently hang. The Vagrant guest sets
`timestamp_timeout=-1` for the `vagrant` user explicitly in
`/etc/sudoers.d/90-vagrant-nopasswd`, defeating any reprovisioned
inheritance from the host.

### 10.5 The `AF_UNSPEC` conntrack dump lie

`nfct_query(NFCT_Q_DUMP, AF_UNSPEC)` returns only `AF_INET` on
many kernel/libnetfilter_conntrack combinations. Always issue
**separate `AF_INET` and `AF_INET6` dumps**. Same trap exists for
sock_diag dumps if you're tempted to ask for `AF_UNSPEC` there
(don't).

### 10.6 Devbox podman won't run rootful

On Debian dev hosts with systemd-logind quirks (RLIMIT delegation),
even `sudo podman run` fails with `error setting rlimit type 7`.
This won't bite a fresh ubuntu CI runner (no logind in the
container-style runner image). The local-integration runner skips
cleanly (`exit 77`) on environment failure rather than reporting
red.

---

## 11. What we don't (yet) attribute

* **Kernel sockets** (NFS, CIFS, etc.). They have no PID. We'd need
  to label by a separate convention.
* **Just-died processes.** If the process closed the socket and
  exited before our resolver got a chance, we have inode but no fd
  to walk. Drop.
* **Rootless containers behind pasta/slirp4netns.** Correctly
  attributed to the proxy process; we could "peer through" the
  proxy in future, but it requires reading pasta's netns peer
  inode and re-running attribution there.
* **gVisor sandboxes.** The guest kernel runs in userspace; sockets
  inside the sandbox are not visible to host sock_diag.
* **Container *names*** (as opposed to IDs). The runtime CLI knows
  the human name, but resolving it requires shelling out to
  `docker inspect` / `podman inspect`, which we don't do in the
  hot path. `containerName` is therefore empty today; the UI shows the
  runtime plus the short hex ID (or the full human-name ID for
  lxc/lxd/nspawn).

---

## 12. Reading list (the actual primary sources)

* `man 7 namespaces`, `man 2 setns`, `man 2 unshare` — the
  authoritative netns docs. Pay attention to the per-namespace-
  type behaviour table.
* `man 7 sock_diag` — kernel socket monitoring interface. Most
  online tutorials are for `ss(8)` which wraps it; the man page is
  better.
* `man 5 proc` — the `/proc/<pid>/{cgroup,ns,stat,fd}` formats.
* Kernel source: `Documentation/admin-guide/cgroup-v2.rst` for the
  modern cgroup hierarchy; `kernel/cgroup/cgroup.c` if you need to
  understand why a path is what it is.
* Linux kernel commit log around `IFLA_NEW_NETNSID`,
  `NETNSA_NSID` — useful for cross-netns identifier work.
* The `runc`, `crun`, `containerd`, `cri-o` source trees — search
  for `cgroupPath` / `CgroupPath` to find the canonical path
  formats they produce, before relying on regexes.
* `iftop` (the original) and `nethogs` source — instructive for
  what *not* to do (both use `/proc/net/tcp` parsing and skip the
  cross-netns problem entirely).

---

## 13. If you're starting from scratch

Build in this order; each layer is independently shippable and
testable:

1. **sock_diag dump → inode list.** Get the full 4-tuple and
   local-only key → inode maps working on the host netns only. Test
   with `iperf3` to localhost and a UDP listener.
2. **`/proc/<pid>/fd` reverse-walk + starttime stamping.** Now you
   have pid attribution for host processes. Compare against `ss
   -tpn` for ground truth.
3. **`/proc/<pid>/cgroup` parse + regex classifier.** Now you have
   container labels for host-netns flows. Spin up a single docker
   container, observe a flow, verify the label.
4. **Add the next container runtime.** Each new one almost always
   surfaces a regex bug (delimiters, parent slice). Test the regex
   in isolation before integrating.
5. **NetnsScanner — the big one.** Worker thread, anchor netns
   discipline, per-netns netlink sockets. This is where most of
   the "I broke production" potential lives. Do not skip step 4
   first; you want the easy cases working before you start
   debugging cross-netns.
6. **End-to-end probe binary + test harness.** The earlier you
   have a real container in a real CI runner, the more bugs you
   catch before they reach users.

Steps 1–3 are a long weekend. Step 5 is two or three. Steps 6 and
the runtime matrix are perpetual.

---

## 9. Why a flow has *no* process — the attribution reason

`pid == 0` is not always a failure. On a router/NAT host most of the
conntrack table is traffic the host merely forwards, with no local
socket and no local process by design. To stop those from looking like
attribution bugs, every `ConnectionDto` carries a `reason`
(`AttributionReason`, capability `attribution-reason`) computed
server-side right after the resolver runs, reusing the same host-address
context as direction inference (`heuristics::attributionReason`):

| reason | when | meaning |
|--------|------|---------|
| `Resolved` | `pid > 0` | attributed to a local process |
| `Forwarded` | neither endpoint is one of this host's addresses | routed / NAT / masquerade — another machine's flow; there is no local process |
| `Orphaned` | TCP in a teardown state (FIN_WAIT/CLOSE_WAIT/LAST_ACK/TIME_WAIT/CLOSE) | the owning socket is already gone (inode 0) |
| `NoLocalSocket` | local endpoint is ours but no live socket was found | a closed UDP flow still lingering in conntrack, a kernel socket, or a genuine miss |

A PID always wins (`Resolved`), so a netns-scanned container flow that
*looks* forwarded by address still reports `Resolved`. The reason is a
filter field (`reason:forwarded`) and drives the GUI/TUI's synthetic
Process-column label (`— forwarded —`, etc.) so the user sees *why* a
flow is unattributed rather than a bare dash. Clients that talk to an
agent without the `attribution-reason` token (or use the in-process
backend) derive the same value locally from the existing wire fields via
`heuristics::attributionReason`.

Note the boundaries: `Forwarded` and `NoLocalSocket` are the kernel's
"a conntrack flow needn't map to any host fd" cases — forwarded packets
never hit a local socket, and conntrack entries (especially UDP, with
its 30–120 s timeout) routinely outlive the ephemeral socket that
created them. Going *deeper* (per-netns scanning, and the planned
resolver-eagerness tiers) only recovers **local** container/VM/netns
owners; a genuinely remote forwarded flow has no local process to find.
