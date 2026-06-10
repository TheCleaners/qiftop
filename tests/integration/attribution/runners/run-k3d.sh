#!/usr/bin/env bash
#
# run-k3d.sh — Tier-2c attribution probe runner against k3d (k8s).
#
# k3d runs a k3s cluster as docker containers; pods run inside k3s
# using its embedded containerd. From the host's perspective, a pod's
# init process is reachable via /proc/<pid> like any other process —
# but its cgroup path is NESTED (host scope → k3d node container scope
# → kubepods slice → cri-containerd-<id>.scope), and its netns is also
# nested (deeper than a flat docker setup). This is the scenario that
# exercises NetnsScanner's "walk every netns" path most thoroughly.
#
# Acceptable attribution outcomes:
#   * INNERMOST runtime=containerd  (cri-containerd-<id>.scope leaf —
#                                    preferred, what current k3s + the
#                                    systemd-cgroup-driver yields)
#   * INNERMOST runtime=kubernetes  (kubepods.slice/... pod scope fallback —
#                                    what you'd see if the leaf scope uses
#                                    an unrecognised format and the
#                                    kubepods fallback kicks in)
#   * CHAIN must contain a docker wrapper (the k3d node container) AND
#     a containerd-or-kubernetes leaf (the actual pod workload). This
#     is what proves we walked the full nesting and didn't just stop at
#     the outermost container — the bug that prompted leaf-wins +
#     classifyPathChain in the first place (Step 4-followup, 2026-06-xx).
# Anything else means a regex gap.
#
# Requires: docker, k3d, kubectl in PATH; CAP_SYS_ADMIN to run the probe.
# Exit:  77 = SKIP (no k3d / cluster won't start)
#        70 = harness error
#        0/1/2 = probe exit code
set -euo pipefail

readonly CLUSTER="qiftop-attr-$$"
readonly POD_NAME="qiftop-attr-pod"
readonly LISTEN_PORT=18082
readonly PROBE_BIN="${QIFTOP_PROBE_BIN:-}"
readonly KUBECONFIG_TMP="$(mktemp --suffix=.kubeconfig)"

if [[ -z "$PROBE_BIN" || ! -x "$PROBE_BIN" ]]; then
    echo "harness: QIFTOP_PROBE_BIN unset or not executable ('$PROBE_BIN')" >&2
    exit 70
fi
for tool in docker k3d kubectl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "harness: $tool not in PATH; SKIPPING" >&2
        exit 77
    fi
done
if ! docker info >/dev/null 2>&1; then
    echo "harness: docker daemon not reachable; SKIPPING" >&2
    exit 77
fi

LISTENER_LOG=$(mktemp)
PROBE_TMP=$(mktemp)
cleanup() {
    echo "harness: cleanup — deleting cluster $CLUSTER" >&2
    k3d cluster delete "$CLUSTER" >/dev/null 2>&1 || true
    [[ -n "${LISTENER_PID:-}" ]] && kill -9 "$LISTENER_PID" 2>/dev/null || true
    rm -f "$LISTENER_LOG" "$PROBE_TMP" "$KUBECONFIG_TMP"
}
trap cleanup EXIT

export KUBECONFIG="$KUBECONFIG_TMP"

echo "harness: creating k3d cluster '$CLUSTER' (this takes ~30s cold)"
# --no-lb            no load balancer container (we don't need ingress)
# --servers 1        single server node
# --agents 0         no separate agent nodes (pods run on the server)
# --wait             block until the API server is up
# --k3s-arg          quieter logs from k3s itself
if ! k3d cluster create "$CLUSTER" \
        --no-lb --servers 1 --agents 0 \
        --wait --timeout 90s; then
    echo "harness: k3d cluster create failed" >&2
    exit 70
fi

# Host IP reachable from inside the cluster: k3d publishes the host as
# the gateway of its docker network. Resolve it via the server node's
# default route.
SERVER_CTR="k3d-${CLUSTER}-server-0"
HOST_IP="$(docker exec "$SERVER_CTR" ip -4 route get 1.1.1.1 2>/dev/null \
    | awk '/via/ {for (i=1;i<=NF;i++) if ($i=="via") print $(i+1)}' \
    | head -1)"
if [[ -z "$HOST_IP" ]]; then
    echo "harness: could not resolve host IP from inside k3d server" >&2
    exit 70
fi

# Host-side listener BEFORE the pod starts dialling.
setsid bash -c "exec nc -l -p ${LISTEN_PORT} >/dev/null" </dev/null >"$LISTENER_LOG" 2>&1 &
LISTENER_PID=$!
sleep 0.3
if ! ss -tlnH "sport = ${LISTEN_PORT}" 2>/dev/null | grep -q ":${LISTEN_PORT}"; then
    echo "harness: nc listener did not come up on :${LISTEN_PORT} ($(cat "$LISTENER_LOG"))" >&2
    exit 70
fi
echo "harness: host listener bound on ${HOST_IP}:${LISTEN_PORT} (pid=${LISTENER_PID})"

echo "harness: waiting for default ServiceAccount (k3s creates it asynchronously)"
for _ in $(seq 1 30); do
    if kubectl get sa default >/dev/null 2>&1; then break; fi
    sleep 1
done
if ! kubectl get sa default >/dev/null 2>&1; then
    echo "harness: default ServiceAccount never appeared" >&2
    exit 70
fi

# Launch a pod that holds a long-lived TCP outbound to the host.
# Image: alpine (already pre-pulled in the VM provisioner; k3d ships
# an image-import path but it's slower than just pulling fresh).
echo "harness: launching pod $POD_NAME"
kubectl run "$POD_NAME" \
    --image=alpine:3.20 \
    --restart=Never \
    --command -- sh -c \
    "while true; do nc -w 30 ${HOST_IP} ${LISTEN_PORT} </dev/zero >/dev/null; sleep 0.1; done" \
    >/dev/null
kubectl wait --for=condition=Ready "pod/${POD_NAME}" --timeout=60s >/dev/null

# Capture the pod's container ID. k3s reports IDs prefixed by
# "containerd://<64hex>" in the status.
RAW_CID="$(kubectl get pod "$POD_NAME" -o jsonpath='{.status.containerStatuses[0].containerID}')"
CID="${RAW_CID#*://}"
CID_SHORT="${CID:0:12}"
if [[ -z "$CID_SHORT" ]]; then
    echo "harness: could not extract container ID from pod status" >&2
    exit 70
fi
echo "harness: pod container CID = ${CID_SHORT}"

# Wait for the host kernel to see the inbound flow on our listener (this
# is the post-double-NAT shape: <k3d-server-ip>:<port> -> <host>:<port>).
# We use this only to confirm the dial-out is alive; the probe itself
# queries the PRE-NAT 4-tuple as seen INSIDE the pod's netns, because
# that's where the actual socket lives and that's what NetnsScanner
# finds via setns.
for _ in $(seq 1 60); do
    if ss -tnH "sport = ${LISTEN_PORT}" 2>/dev/null | grep -q . ; then break; fi
    sleep 0.5
done
if ! ss -tnH "sport = ${LISTEN_PORT}" 2>/dev/null | grep -q . ; then
    echo "harness: ss did not observe inbound flow on :${LISTEN_PORT} within 30s" >&2
    exit 70
fi

# Ask the pod itself what its outbound 4-tuple looks like. Format from
# `ss -tnH` is: STATE RECVQ SENDQ LOCAL_ADDR:PORT PEER_ADDR:PORT
POD_TUPLE=""
for _ in $(seq 1 30); do
    # Alpine ships only busybox netstat (no ss). Match the destination
    # port and pull the local addr:port (col 4 in 'netstat -tn' output).
    POD_TUPLE="$(kubectl exec "$POD_NAME" -- sh -c \
        "netstat -tn 2>/dev/null | awk -v p=:${LISTEN_PORT}\$ '\$5 ~ p {print \$4; exit}'" 2>/dev/null || true)"
    [[ -n "$POD_TUPLE" ]] && break
    sleep 0.5
done
if [[ -z "$POD_TUPLE" ]]; then
    echo "harness: pod did not show an outbound socket to :${LISTEN_PORT} within 15s" >&2
    exit 70
fi
LOCAL_TUPLE="${HOST_IP}:${LISTEN_PORT}"
echo "harness: pod-internal flow tuple = ${POD_TUPLE} -> ${LOCAL_TUPLE}"

# Note: we don't pin --expect-container-id-prefix for k3d. The CID we
# extract from kubectl is the pod's pause/init container in containerd's
# numbering — the netns holder we attribute may legitimately be a sibling
# container in the same pod sandbox with a different CID. Asserting only
# on runtime keeps the test honest without being flaky.
set +e
"$PROBE_BIN" --json \
    --proto tcp \
    --local  "${POD_TUPLE}" \
    --remote "${LOCAL_TUPLE}" \
    --expect-container-runtime containerd,kubernetes \
    --expect-chain-min-depth 2 \
    --expect-chain-contains docker \
    --expect-chain-contains containerd,kubernetes \
    --timeout-ms 15000 | tee "$PROBE_TMP"
PROBE_RC=$?
set -e

exit "$PROBE_RC"
