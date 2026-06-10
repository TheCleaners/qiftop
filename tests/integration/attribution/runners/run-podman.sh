#!/usr/bin/env bash
#
# run-podman.sh — Tier-2a attribution probe runner against Podman.
#
# Uses ROOTFUL podman (sudo podman) because:
#   * Rootless+pasta does usermode networking — outbound flows on the
#     host side are held by the pasta(1) process, not by anything with
#     a podman cgroup. Attribution would correctly point at pasta, not
#     at a container.
#   * Rootless+slirp4netns has the same problem with slirp4netns(1).
#   * Rootful uses netavark/CNI with a real Linux bridge, so the flow
#     genuinely lives inside the container's netns and NetnsScanner can
#     find it via setns(CLONE_NEWNET).
#
# What this exercises that the docker runner doesn't:
#   rxPodman match on a `libpod-<64hex>.scope` cgroup path under
#   /machine.slice/ — produced by rootful podman with the systemd
#   cgroup manager. A real-world container hierarchy.
#
# If `sudo podman` can't start a container (e.g. on a dev box with
# logind quirks like rlimit delegation issues), the runner exits 77
# (= ctest SKIP) rather than failing.

set -euo pipefail

readonly IMAGE="${QIFTOP_PROBE_IMAGE:-docker.io/library/alpine:3.20}"
readonly NAME="qiftop-attr-probe-$$"
readonly LISTEN_PORT=18081
readonly PROBE_BIN="${QIFTOP_PROBE_BIN:-}"

# Guard: an image ref starting with '-' could be parsed as a podman CLI
# option instead of an image name.
if [[ "$IMAGE" == -* ]]; then
    echo "harness: QIFTOP_PROBE_IMAGE must not start with '-' (got '$IMAGE')" >&2
    exit 70
fi

if [[ -z "$PROBE_BIN" || ! -x "$PROBE_BIN" ]]; then
    echo "harness: QIFTOP_PROBE_BIN unset or not executable ('$PROBE_BIN')" >&2
    exit 70
fi
if ! command -v podman >/dev/null 2>&1; then
    echo "harness: podman not in PATH; SKIPPING" >&2
    exit 77
fi

PODMAN=(podman)
if [[ $EUID -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "harness: rootful podman needs root or sudo; SKIPPING" >&2
        exit 77
    fi
    PODMAN=(sudo --preserve-env=PATH podman)
fi

# Sanity-check that rootful podman can actually start a container on
# this host. If `podman run` fails for environmental reasons (rlimit
# delegation, missing kernel module, etc.), skip rather than report a
# false failure of the qiftop attribution layer.
if ! "${PODMAN[@]}" run --rm "$IMAGE" true >/dev/null 2>&1; then
    echo "harness: rootful podman cannot start containers on this host; SKIPPING" >&2
    exit 77
fi

HOST_IP="$("${PODMAN[@]}" network inspect podman \
    --format '{{range .Subnets}}{{.Gateway}}{{end}}' 2>/dev/null || true)"
if [[ -z "$HOST_IP" || "$HOST_IP" == "<no value>" ]]; then
    HOST_IP="$(ip -4 -o addr show 2>/dev/null \
        | awk '/podman0|cni-podman0/ {print $4}' | head -1 | cut -d/ -f1)"
fi
if [[ -z "$HOST_IP" ]]; then
    echo "harness: cannot determine podman bridge gateway IP" >&2
    exit 70
fi

LISTENER_LOG=$(mktemp)
PROBE_OUT=$(mktemp)
cleanup() {
    "${PODMAN[@]}" rm -f "$NAME" >/dev/null 2>&1 || true
    [[ -n "${LISTENER_PID:-}" ]] && kill -9 "$LISTENER_PID" 2>/dev/null || true
    rm -f "$LISTENER_LOG" "$PROBE_OUT"
}
trap cleanup EXIT

setsid bash -c "exec nc -l -p ${LISTEN_PORT} >/dev/null" </dev/null >"$LISTENER_LOG" 2>&1 &
LISTENER_PID=$!
sleep 0.3
if ! ss -tlnH "sport = ${LISTEN_PORT}" 2>/dev/null | grep -q ":${LISTEN_PORT}"; then
    echo "harness: nc listener did not come up on :${LISTEN_PORT} (log: $(cat "$LISTENER_LOG"))" >&2
    exit 70
fi
echo "harness: host listener on ${HOST_IP}:${LISTEN_PORT}"

"${PODMAN[@]}" run -d --rm --name "$NAME" "$IMAGE" \
    sh -c "while true; do nc -w 30 ${HOST_IP} ${LISTEN_PORT} </dev/zero >/dev/null; sleep 0.1; done" \
    >/dev/null

CID=""
for _ in $(seq 1 30); do
    CID="$("${PODMAN[@]}" inspect -f '{{.Id}}' "$NAME" 2>/dev/null || true)"
    [[ -n "$CID" ]] && break
    sleep 0.2
done
if [[ -z "$CID" ]]; then
    echo "harness: container did not start within 6s" >&2
    exit 70
fi
CID_SHORT="${CID:0:12}"
echo "harness: container ${CID_SHORT} dialling out"

REMOTE_TUPLE=""
for _ in $(seq 1 50); do
    REMOTE_TUPLE="$(ss -tnH "sport = ${LISTEN_PORT}" 2>/dev/null \
        | awk '{print $5; exit}')"
    if [[ -n "$REMOTE_TUPLE" ]]; then break; fi
    sleep 0.2
done
if [[ -z "$REMOTE_TUPLE" ]]; then
    echo "harness: ss did not observe an inbound flow on :${LISTEN_PORT}" >&2
    exit 70
fi
LOCAL_TUPLE="${HOST_IP}:${LISTEN_PORT}"
echo "harness: observed flow ${REMOTE_TUPLE} -> ${LOCAL_TUPLE}"

PROBE_CMD=("$PROBE_BIN")
if [[ $EUID -ne 0 ]]; then
    PROBE_CMD=(sudo --preserve-env=PATH "$PROBE_BIN")
fi

set +e
"${PROBE_CMD[@]}" --json \
    --proto tcp \
    --local  "${REMOTE_TUPLE}" \
    --remote "${LOCAL_TUPLE}" \
    --expect-container-runtime podman \
    --expect-container-id-prefix "${CID_SHORT}" \
    --timeout-ms 10000 | tee "$PROBE_OUT"
PROBE_RC=$?
set -e

exit "$PROBE_RC"
