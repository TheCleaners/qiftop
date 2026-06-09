# Attribution integration tests (Tier 2)

End-to-end harness that drives a real container runtime, generates a
flow originating *inside* a container, and asks the production
`qiftop::backend::createProcessResolver()` chain to attribute the flow
back to the container's runtime + id.

This is the dynamic counterpart to `tests/test_cgroup_real_fixtures.cpp`
(Tier 1 — pure cgroup-path parsing against fixtures captured from
upstream docs). Tier 1 protects the parsers; Tier 2 protects the
sock_diag dump, NetnsScanner setns dance, and CompositeResolver fan-out
against real runtimes.

## Status

| Runner | Status                                         |
|--------|------------------------------------------------|
| docker | **Shipped** (`runners/run-docker.sh`)          |
| podman | Planned                                        |
| k3d    | Planned                                        |
| cri-o  | Planned                                        |

## Layout

```
tests/integration/attribution/
├── README.md
├── CMakeLists.txt                          gated by QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON
├── driver/qiftop-attribution-probe.cpp     C++ probe binary (resolver in-process, CLI contract below)
└── runners/run-docker.sh                   per-runtime shell harness
```

## Probe binary contract

```
qiftop-attribution-probe
    --proto tcp|udp
    --local  <ip>:<port>
    --remote <ip>:<port>
    [--expect-pid <int>]
    [--expect-pid-comm <name>]
    [--expect-container-runtime <name>]
    [--expect-container-id-prefix <hex>]
    [--timeout-ms <int>]   (default 8000)
    [--poll-ms <int>]      (default 250)
    [--json]
```

Exit codes: `0`=match, `1`=mismatch, `2`=timeout, `3`=argv error.
Emits a JSON line on stdout describing the attribution + any failed
expectations. Reusable by any runner.

## Running locally

```
# One-shot driver: reconfigures, rebuilds the probe, runs ctest under
# sudo (NetnsScanner needs CAP_SYS_ADMIN).
scripts/integration-test.sh --runtime docker

# Or manually:
cmake -S . -B build -DQIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON
cmake --build build --target qiftop-attribution-probe
sudo ctest --test-dir build -L attribution-integration --output-on-failure -V
```

## CI

`.github/workflows/integration.yml` runs the docker runner on
`push to main` + `workflow_dispatch` + published releases ONLY. It is
deliberately NOT triggered on every PR / feature-branch push — runners
take minutes and require docker daemon access.

## Why this isn't part of default ctest

* Requires root or CAP_NET_ADMIN+CAP_SYS_ADMIN to dump sock_diag and
  `setns()` into container netns.
* Requires a container runtime daemon + image pulls.
* Takes 5-10 s per runner (vs. ms for unit tests).

`QIFTOP_BUILD_ATTRIBUTION_INTEGRATION` defaults to `OFF`. Even when ON,
the docker runner returns exit code 77 (= ctest SKIP) when `docker info`
fails, so a partial setup doesn't fail the suite spuriously.

## What this proves end-to-end

A container-internal `nc` opens an outbound TCP connection to a
host-side listener. The probe asks the resolver about that 4-tuple
(observed via host-side `ss -tn`) and expects:

* `container.runtime == "docker"`
* `container.id` starts with the container's first 12 hex chars

NetnsScanner is the component that makes this work — sock_diag in the
host netns alone can't see the container's outbound socket. A
regression in setns/CLONE_NEWNET handling, in cgroup-path parsing
for whatever runtime the CI image ships, or in flow-key symmetry
inside CompositeResolver, will fail this test.
