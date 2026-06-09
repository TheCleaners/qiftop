#!/usr/bin/env bash
#
# integration-test.sh — dev-box driver for the Tier-2 attribution harness.
#
# Reconfigures the build with QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON, rebuilds
# just the probe binary, and runs the chosen runner via ctest. Idempotent.
#
# Usage:
#   scripts/integration-test.sh [--runtime docker] [--build-dir build]
#                               [--keep] [-v|--verbose]
#
# Requires:
#   - cmake, ninja or make, a working C++ toolchain
#   - the runtime you ask for (currently only docker is implemented)
#   - root / sudo for the probe (it talks to /proc and netlink)
#
# This is NOT part of the default ctest set. CI runs the same harness via
# .github/workflows/integration.yml on push-to-main / release / dispatch.

set -euo pipefail

RUNTIME="docker"
BUILD_DIR="build"
KEEP=0
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runtime)   RUNTIME="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --keep)      KEEP=1; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        -h|--help)
            sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

case "$RUNTIME" in
    docker|podman|k3d|k8s) ;;
    crio)
        echo "runtime '$RUNTIME' planned but not yet implemented; supported: docker, podman, k3d, k8s" >&2
        exit 2 ;;
    *) echo "unknown runtime '$RUNTIME'" >&2; exit 2 ;;
esac

# `k8s` is satisfied by `k0s` on the host; the binary on PATH check below
# uses the runner-side tool name, so map and skip the host check for it.
HOST_TOOL="$RUNTIME"
if [[ "$RUNTIME" == "k8s" ]]; then HOST_TOOL="k0s"; fi
if ! command -v "$HOST_TOOL" >/dev/null 2>&1; then
    echo "$HOST_TOOL not in PATH" >&2; exit 2
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo ">> reconfiguring $BUILD_DIR with QIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON"
cmake -S . -B "$BUILD_DIR" \
    -DQIFTOP_BUILD_TESTS=ON \
    -DQIFTOP_BUILD_INTEGRATION_TESTS=ON \
    -DQIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON \
    ${VERBOSE:+--log-level=VERBOSE}

echo ">> building qiftop-attribution-probe"
cmake --build "$BUILD_DIR" --target qiftop-attribution-probe -j"$(nproc)"

CTEST_ARGS=(--test-dir "$BUILD_DIR" -R "attribution_${RUNTIME}" --output-on-failure)
if [[ $VERBOSE -eq 1 ]]; then CTEST_ARGS+=(-V); fi

if [[ $EUID -ne 0 ]]; then
    echo ">> not root; re-invoking ctest under sudo (probe needs CAP_NET_ADMIN+CAP_SYS_ADMIN)"
    sudo --preserve-env=PATH ctest "${CTEST_ARGS[@]}"
else
    ctest "${CTEST_ARGS[@]}"
fi

if [[ $KEEP -eq 0 ]]; then
    echo ">> harness cleanup handled by EXIT trap in run-${RUNTIME}.sh"
fi
