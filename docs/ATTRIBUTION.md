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
| `comm` / `exe`     | `/proc/<pid>/{comm,exe}`                          |
| `uid`              | sock_diag `idiag_uid`                             |
| `container.runtime`| classify `/proc/<pid>/cgroup` line                |
| `container.id`     | the 64-hex blob in the cgroup path                |
| `container.name`   | runtime-specific lookup (often skipped — costly)  |

Three properties matter:

1. **Best-effort, not authoritative.** A flow may legitimately have
   no resolvable pid (kernel sockets, just-died process, foreign netns
   we can't enter). Every field is `optional`.
2. **Stale-by-construction.** PIDs are reused. We snapshot the
   process's `starttime` jiffies at enrollment and re-check at lookup;
   a mismatch means the cache is poisoned and we drop the answer
   rather than misattribute (`ProcSnapshot::pidStartTime`,
   `SockDiagResolver.cpp:147`).
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
just disables `NetnsScanner` — the rest keep working).

---

## 4. Data source #1 — `NETLINK_SOCK_DIAG`

This is the modern replacement for parsing `/proc/net/tcp`. It is:

* **Atomic-ish.** A single dump returns a consistent snapshot at the
  netlink layer's reader-writer lock; `/proc/net/tcp` walks the
  socket hash table without one.
* **Cheap.** Milliseconds even with tens of thousands of sockets.
* **Privileged for cross-user lookups.** Unprivileged callers get
  their own sockets; full visibility needs `CAP_NET_ADMIN` (or root).

What it returns per socket: the 4-tuple, the inet UID, and crucially
the **kernel inode of the socket** (`idiag_inode`). The inode is the
join key for the next step.

What it does **not** return: the owning PID. Linux does not maintain
socket→pid in-kernel because sockets are fds (multiple processes can
own the same fd via `dup`/fork/SCM_RIGHTS).

### Reverse-mapping socket inode → pid

We walk `/proc/<pid>/fd/*`, `readlink`-ing each one. Targets that
match `socket:[<inode>]` give us a row in a `inode → pid` map.

Lessons:

* `QDir::Files` filters by `S_ISREG` of the symlink target, which an
  unresolved `socket:[…]` link is not. Use `QDir::System` instead
  (`SockDiagResolver.cpp:118`).
* If a PID dies between starttime read and fd walk, just drop it;
  don't propagate the error (`SockDiagResolver.cpp:65`).
* **Snapshot the starttime at enrollment**, store it next to the pid,
  re-read at lookup time. If they differ, the kernel reused the pid
  for a fresh process and your cache is lying — return `nullopt`,
  don't return the wrong cmdline.

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

`extractPath()` (`CgroupParse.h:24`) handles both: prefer the v2 line
(`0::/path`); fall back to any v1 line if v2 isn't present. **Every
modern Linux has v2**; the v1 fallback exists for old RHEL 7-era
hosts where qiftop might be installed for portability.

### The runtime regex cookbook

We classify the path against a table of regexes (`classifyPath`,
`CgroupParse.h:60`). The patterns matter more than they look — each
one was discovered the hard way:

| Runtime    | Pattern (real example)                                                | Notes |
|------------|------------------------------------------------------------------------|-------|
| docker     | `/system.slice/docker-<64hex>.scope` <br> `/docker/<64hex>`            | Both shapes exist; systemd cgroup driver vs cgroupfs driver. |
| containerd | `cri-containerd-<64hex>.scope`                                         | k8s-on-containerd. |
| cri-o      | `/kubepods.slice/.../crio-<64hex>.scope`                               | The `(?:^\|/)` prefix is required — without it `pkrio-…` would match. |
| kubernetes | `kubepods[./].*?pod<32-72hex_underscores>`                             | Pod-level fallback when no runtime-specific scope is present. Underscores in the class because systemd-escaped UIDs are `665b0949_7b83_…`, NOT dashes. |
| podman     | `libpod-<64hex>.scope`                                                 | Same shape rootful or rootless; differs by parent slice (`machine.slice` vs `user.slice/.../user@N.service/user.slice/`). |
| lxd        | `/system.slice/lxd-<name>.service/lxc.payload`                         | Match the `.service` segment; tested before generic systemd unit so LXD wins. |
| lxc        | `/lxc(\.payload)?[./]<name>`                                           | Plain LXC, names not hex. |
| systemd    | `/<unit>.{service,socket,mount}`                                       | Non-container scope but useful — UI labels it `unit:nginx.service`. |

Order matters in `classifyPath`: lxd before generic systemd, kubepods
fallback after the runtime-specific scopes, etc.

**The 12-char short ID** is what every CLI shows (`docker ps`,
`podman ps`); we slice it as `id.left(12)` everywhere to match what a
sysadmin pastes from elsewhere.

### Host-vs-container heuristic

These cgroup paths mean **the host**, not a container:

* empty
* `/`
* `/init.scope`

`classifyPath` returns `nullopt` for them. Anything else gets a label
(possibly just `unit:foo.service`).

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

### Failure modes (all routine, all silent)

| Error                           | Cause                                       | Action |
|---------------------------------|---------------------------------------------|--------|
| `ENOENT` reading `/proc/X/ns/net` | pid vanished between readdir and open    | skip   |
| `EPERM` on setns                | not CAP_SYS_ADMIN                           | scanner disabled in `initialize()` |
| `EINVAL` on setns               | netns destroyed mid-walk                    | skip   |
| sock_diag dump fails post-setns | netns has no inet (unusual)                 | log once per cycle, continue |
| setns(anchor) restore fails     | catastrophic — anchor netns gone            | `qFatal`, agent dies cleanly |

The rule is: **anything except restore failure is routine**. Don't
spam logs. (`NetnsScanner.cpp:186-197`)

---

## 7. Race conditions — the four we actually defend against

Attribution is *all* races. The interesting ones, and how we handle
each:

### 7.1 PID reuse

The kernel's pid space is small (32k by default, 4M with
`/proc/sys/kernel/pid_max` bumped). A long-lived agent will see pid
recycling within minutes on a busy host.

**Defence:** snapshot `/proc/<pid>/stat` field 22 (starttime in
jiffies since boot) at enrollment, store it next to the pid in the
inode→pid map. At lookup, re-read and compare; mismatch → drop the
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
  hot path. The UI currently shows the 12-char ID.

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

1. **sock_diag dump → inode list.** Get the 4-tuple → inode map
   working on the host netns only. Test with `iperf3` to localhost.
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
