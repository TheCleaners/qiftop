#!/usr/bin/env bash
#
# run-crio.sh — Tier-2 attribution probe runner against CRI-O.
#
# CRI-O is CRI-only: there is no docker/podman-style "run this shell"
# shorthand. The harness therefore creates a pod sandbox, creates a
# container in that sandbox from JSON configs, starts it, then uses
# crictl exec to read the container-internal socket tuple.
#
# The flow generator deliberately dials an EXTERNAL host:
#   sleep 3600 | nc <resolved-ip> 22
# That yields one long-lived, conntracked TCP socket with a stable owner PID.
# Host-local bridge traffic is not conntracked, HTTPS via busybox wget lacks
# TLS, reconnect loops churn PIDs, and detached nc exits on EOF — see
# AGENTS.md §6.5b.
#
# Requires:
#   - crictl and a reachable CRI-O socket (/run/crio/crio.sock by default)
#   - root / sudo / CAP_NET_ADMIN+CAP_SYS_ADMIN for crictl and the probe
#   - outbound TCP to QIFTOP_PROBE_TARGET_HOST:QIFTOP_PROBE_TARGET_PORT
#     (default github.com:22)
#   - QIFTOP_PROBE_BIN env var pointing at qiftop-attribution-probe
#
# Exit: probe's exit code (0=ok, 1=mismatch, 2=timeout). 70=harness error.
#       77=skip (no CRI-O/crictl, no egress, or runtime cannot start).

set -euo pipefail

readonly IMAGE="${QIFTOP_PROBE_IMAGE:-docker.io/library/alpine:3.20}"
readonly TARGET_HOST="${QIFTOP_PROBE_TARGET_HOST:-github.com}"
readonly TARGET_PORT="${QIFTOP_PROBE_TARGET_PORT:-22}"
readonly CRIO_ENDPOINT="${QIFTOP_CRIO_ENDPOINT:-unix:///run/crio/crio.sock}"
readonly SANDBOX_NAME="qiftop-crio-sandbox-$$"
readonly CONTAINER_NAME="qiftop-crio-container-$$"
readonly PROBE_BIN="${QIFTOP_PROBE_BIN:-}"
readonly WORK_DIR="${PWD}/.qiftop-crio-$$"

skip() { echo "harness: $1; SKIPPING" >&2; exit 77; }
die()  { echo "harness: $1" >&2; exit 70; }

[[ "$IMAGE" == -* ]] && die "QIFTOP_PROBE_IMAGE must not start with '-' (got '$IMAGE')"
[[ "$IMAGE" =~ ^[A-Za-z0-9._:/@-]+$ ]] \
    || die "QIFTOP_PROBE_IMAGE contains unsupported characters for JSON config (got '$IMAGE')"
[[ "$TARGET_HOST" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] \
    || die "QIFTOP_PROBE_TARGET_HOST is not a plausible hostname/IP (got '$TARGET_HOST')"
[[ "$TARGET_PORT" =~ ^[0-9]{1,5}$ ]] \
    || die "QIFTOP_PROBE_TARGET_PORT must be numeric (got '$TARGET_PORT')"
if (( TARGET_PORT < 1 || TARGET_PORT > 65535 )); then
    die "QIFTOP_PROBE_TARGET_PORT out of range (got '$TARGET_PORT')"
fi

if [[ -z "$PROBE_BIN" || ! -x "$PROBE_BIN" ]]; then
    die "QIFTOP_PROBE_BIN unset or not executable ('$PROBE_BIN')"
fi
command -v crictl >/dev/null 2>&1 || skip "crictl not in PATH"

if [[ "$CRIO_ENDPOINT" == unix://* ]]; then
    CRIO_SOCK="${CRIO_ENDPOINT#unix://}"
    [[ -S "$CRIO_SOCK" ]] || skip "CRI-O socket ${CRIO_SOCK} missing"
fi

CRICTL=(crictl --runtime-endpoint "$CRIO_ENDPOINT" --image-endpoint "$CRIO_ENDPOINT")
PROBE_CMD=("$PROBE_BIN")
if [[ $EUID -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || skip "root or sudo required for CRI-O/probe access"
    sudo -n true 2>/dev/null || skip "passwordless sudo unavailable"
    CRICTL=(sudo --preserve-env=PATH crictl --runtime-endpoint "$CRIO_ENDPOINT" --image-endpoint "$CRIO_ENDPOINT")
    PROBE_CMD=(sudo --preserve-env=PATH "$PROBE_BIN")
fi

if ! "${CRICTL[@]}" info >/dev/null 2>&1; then
    skip "crictl cannot talk to CRI-O at ${CRIO_ENDPOINT}"
fi

TARGET_IP="$(getent ahostsv4 "$TARGET_HOST" 2>/dev/null | awk '{print $1; exit}' || true)"
if [[ -z "$TARGET_IP" ]] && command -v python3 >/dev/null 2>&1; then
    TARGET_IP="$(python3 -c 'import socket,sys; print(socket.gethostbyname(sys.argv[1]))' \
        "$TARGET_HOST" 2>/dev/null || true)"
fi
[[ -n "$TARGET_IP" ]] || skip "could not resolve ${TARGET_HOST} to an IPv4 address"
if ! timeout 5 bash -c "exec 3<>/dev/tcp/${TARGET_IP}/${TARGET_PORT}" 2>/dev/null; then
    skip "no outbound TCP to ${TARGET_IP}:${TARGET_PORT} (${TARGET_HOST})"
fi

POD_ID=""
CID=""
cleanup() {
    if [[ -n "${CID:-}" ]]; then
        "${CRICTL[@]}" stop "$CID" >/dev/null 2>&1 || true
        "${CRICTL[@]}" rm "$CID" >/dev/null 2>&1 || true
    fi
    if [[ -n "${POD_ID:-}" ]]; then
        "${CRICTL[@]}" stopp "$POD_ID" >/dev/null 2>&1 || true
        "${CRICTL[@]}" rmp "$POD_ID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

mkdir -p "$WORK_DIR" || die "could not create work dir ${WORK_DIR}"
SANDBOX_CFG="${WORK_DIR}/sandbox.json"
CONTAINER_CFG="${WORK_DIR}/container.json"

cat >"$SANDBOX_CFG" <<JSON
{
  "metadata": {
    "name": "${SANDBOX_NAME}",
    "uid": "${SANDBOX_NAME}",
    "namespace": "qiftop",
    "attempt": 1
  },
  "log_directory": "${WORK_DIR}",
  "linux": {}
}
JSON

cat >"$CONTAINER_CFG" <<JSON
{
  "metadata": {
    "name": "${CONTAINER_NAME}",
    "attempt": 1
  },
  "image": {
    "image": "${IMAGE}"
  },
  "command": [
    "sh",
    "-c",
    "sleep 3600 | nc ${TARGET_IP} ${TARGET_PORT}"
  ],
  "log_path": "${CONTAINER_NAME}.log",
  "stdin": false,
  "stdin_once": false,
  "tty": false,
  "linux": {}
}
JSON

echo "harness: pulling ${IMAGE} via CRI-O"
if ! "${CRICTL[@]}" pull "$IMAGE" >/dev/null 2>&1; then
    skip "crictl pull failed for ${IMAGE}"
fi

echo "harness: creating CRI-O pod sandbox ${SANDBOX_NAME}"
if ! POD_ID="$("${CRICTL[@]}" runp "$SANDBOX_CFG" 2>/dev/null)"; then
    skip "crictl runp failed (CRI-O/CNI sandbox unavailable)"
fi
POD_ID="$(printf '%s\n' "$POD_ID" | awk 'NF {print $1; exit}')"
[[ -n "$POD_ID" ]] || skip "crictl runp returned no sandbox ID"

echo "harness: creating CRI-O container ${CONTAINER_NAME}"
if ! CID="$("${CRICTL[@]}" create "$POD_ID" "$CONTAINER_CFG" "$SANDBOX_CFG" 2>/dev/null)"; then
    skip "crictl create failed"
fi
CID="$(printf '%s\n' "$CID" | awk 'NF {print $1; exit}')"
[[ -n "$CID" ]] || skip "crictl create returned no container ID"
CID_SHORT="${CID:0:12}"

if ! "${CRICTL[@]}" start "$CID" >/dev/null 2>&1; then
    skip "crictl start failed"
fi
echo "harness: container ${CID_SHORT} dialing ${TARGET_IP}:${TARGET_PORT} (${TARGET_HOST})"

CONTAINER_TUPLE=""
for _ in $(seq 1 40); do
    CONTAINER_TUPLE="$("${CRICTL[@]}" exec "$CID" sh -c \
        "netstat -tn 2>/dev/null | awk -v tgt='${TARGET_IP}:${TARGET_PORT}' '\$5 == tgt {print \$4; exit}'" \
        2>/dev/null || true)"
    [[ -n "$CONTAINER_TUPLE" ]] && break
    sleep 0.5
done
if [[ -z "$CONTAINER_TUPLE" ]]; then
    skip "container did not show a live TCP socket to ${TARGET_IP}:${TARGET_PORT}"
fi

REMOTE_TUPLE="${TARGET_IP}:${TARGET_PORT}"
echo "harness: container-internal flow tuple = ${CONTAINER_TUPLE} -> ${REMOTE_TUPLE}"

set +e
"${PROBE_CMD[@]}" --json \
    --proto tcp \
    --local  "${CONTAINER_TUPLE}" \
    --remote "${REMOTE_TUPLE}" \
    --expect-container-runtime cri-o \
    --expect-container-id-prefix "${CID_SHORT}" \
    --timeout-ms 15000
PROBE_RC=$?
set -e

exit "$PROBE_RC"
