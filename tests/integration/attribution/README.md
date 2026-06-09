# Attribution integration tests (Tier 2)

Not yet implemented. Tracked in AGENTS.md §6.3 #3.

## Goal

Bring up a real container under each supported runtime, generate a
network flow originating from inside that container, and assert that
`qiftop::backend::createProcessResolver()` returns the expected
`(pid, ProcessInfo, ContainerInfo)` for that flow's 4-tuple.

## Why this is necessary beyond Tier 1

Tier 1 (`tests/test_cgroup_real_fixtures.cpp`) covers cgroup-path
parsing in isolation, using `/proc/<pid>/cgroup` content harvested from
upstream documentation. It does NOT exercise:

- The `SockDiagResolver`'s actual netlink dump against a live socket.
- The `NetnsScanner`'s `setns()` machinery against a real container
  netns.
- The composite resolver's fan-out — does it actually return both
  pid AND container-id for the SAME flow?
- Per-runtime kernel/userspace surprises (e.g. Docker's userland-proxy
  rewriting flows in ways that confuse sock_diag).

## Planned layout

```
tests/integration/attribution/
├── README.md           (this file)
├── compose.docker.yml  (nginx in a default bridge network)
├── compose.podman.yml  (rootless podman pod)
├── k3d.yaml            (k3d cluster + a single-pod deployment)
├── driver/             (the C++ test that drives qiftop's resolver)
└── CMakeLists.txt      (gated by QIFTOP_BUILD_INTEGRATION_TESTS=ON)
```

## Why it isn't built yet

- Docker / Podman / k3d add ~2-3 GB of dependencies to the CI image
  and ~3-5 minutes to test wall time per matrix entry.
- The v0.2 attribution work needs to ship to users before we invest
  in this harness — Tier 1 plus the live smoke test from the
  developer machine is good enough to catch the obvious regressions.

## When to build it

Once step 5 (UI integration) lands and the attribution columns are
visible to users, an attribution regression has user-visible cost.
That's the moment to add this harness.
