#!/usr/bin/env bash
#
# run-docker.sh — Tier-2a attribution probe runner against Docker.
#
# Mirrors qiftop's motivating use case: a process *inside* a container
# initiates an outbound flow. We launch a host-side listener, then a
# container-side client that connects to it. The probe must attribute
# the flow to the docker runtime + container ID via NetnsScanner.
#
# Requires:
#   - docker reachable by the invoking user
#   - root / sudo / CAP_NET_ADMIN+CAP_SYS_ADMIN to run the probe (NetnsScanner
#     needs CAP_SYS_ADMIN for setns(CLONE_NEWNET))
#   - QIFTOP_PROBE_BIN env var pointing at qiftop-attribution-probe
#     (set automatically by the ctest entry)
#
# Exit: probe's exit code (0=ok, 1=mismatch, 2=timeout). 70=harness error.
#       77=skip (no docker available).

set -euo pipefail

readonly IMAGE="${QIFTOP_PROBE_IMAGE:-alpine:3.20}"
readonly NAME="qiftop-attr-probe-$$"
readonly LISTEN_PORT=18080
readonly PROBE_BIN="${QIFTOP_PROBE_BIN:-}"

# Guard: an image ref starting with '-' could be parsed as a docker CLI
# option instead of an image name.
if [[ "$IMAGE" == -* ]]; then
    echo "harness: QIFTOP_PROBE_IMAGE must not start with '-' (got '$IMAGE')" >&2
    exit 70
fi

if [[ -z "$PROBE_BIN" || ! -x "$PROBE_BIN" ]]; then
    echo "harness: QIFTOP_PROBE_BIN unset or not executable ('$PROBE_BIN')" >&2
    exit 70
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "harness: docker not in PATH; SKIPPING" >&2
    exit 77
fi
if ! docker info >/dev/null 2>&1; then
    echo "harness: docker daemon not reachable; SKIPPING" >&2
    exit 77
fi

# Pick the host's docker0 bridge IP so the container can reach back.
HOST_IP="$(ip -4 -o addr show docker0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)"
if [[ -z "$HOST_IP" ]]; then
    echo "harness: docker0 has no IPv4; is docker really running?" >&2
    exit 70
fi

LISTENER_LOG=$(mktemp)
PROBE_TMP=$(mktemp)
cleanup() {
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    [[ -n "${LISTENER_PID:-}" ]] && kill -9 "$LISTENER_PID" 2>/dev/null || true
    rm -f "$LISTENER_LOG" "$PROBE_TMP"
}
trap cleanup EXIT

# Host-side listener that just sits on the port.
setsid bash -c "exec nc -l -p ${LISTEN_PORT} >/dev/null" </dev/null >"$LISTENER_LOG" 2>&1 &
LISTENER_PID=$!
sleep 0.3
if ! ss -tlnH "sport = ${LISTEN_PORT}" 2>/dev/null | grep -q ":${LISTEN_PORT}"; then
    echo "harness: nc listener did not come up on :${LISTEN_PORT} (log: $(cat "$LISTENER_LOG"))" >&2
    exit 70
fi
echo "harness: host listener bound on ${HOST_IP}:${LISTEN_PORT} (pid=${LISTENER_PID})"

# Container-side initiator. We pass --label so the probe's expectations
# stay tied to a known image (CID comes back from docker inspect).
docker run -d --rm --name "$NAME" "$IMAGE" \
    sh -c "while true; do nc -w 30 ${HOST_IP} ${LISTEN_PORT} </dev/zero >/dev/null; sleep 0.1; done" \
    >/dev/null

CID=""
for _ in $(seq 1 20); do
    CID="$(docker inspect -f '{{.Id}}' "$NAME" 2>/dev/null || true)"
    [[ -n "$CID" ]] && break
    sleep 0.2
done
if [[ -z "$CID" ]]; then
    echo "harness: container did not start within 4s" >&2
    exit 70
fi
CID_SHORT="${CID:0:12}"
echo "harness: container ${CID_SHORT} dialling ${HOST_IP}:${LISTEN_PORT}"

# Wait for the kernel to see the inbound flow on the host listener side,
# so we can grab the container's ephemeral source port.
REMOTE_TUPLE=""
for _ in $(seq 1 30); do
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

# The probe asks the resolver about the flow from the *container's*
# perspective — local is the container's source, remote is the host
# listener. NetnsScanner will discover it inside the container netns.
set +e
"$PROBE_BIN" --json \
    --proto tcp \
    --local  "${REMOTE_TUPLE}" \
    --remote "${LOCAL_TUPLE}" \
    --expect-container-runtime docker \
    --expect-container-id-prefix "${CID_SHORT}" \
    --timeout-ms 10000 | tee "$PROBE_TMP"
PROBE_RC=$?
set -e

exit "$PROBE_RC"
