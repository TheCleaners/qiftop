#!/usr/bin/env bash
#
# run-k8s.sh — Tier-2d attribution probe runner against NAKED Kubernetes.
#
# "Naked" here means k8s running directly on the host (no docker-in-docker
# wrapper like k3d). We use k0s in --single mode — single binary, runs as
# a systemd service, ships containerd embedded. This is the cgroup path
# shape a real production k8s node would show: no outer docker scope,
# just /kubepods/<qos>/pod<UID>/<64hex-containerd-leaf> (cgroupfs driver)
# or /kubepods.slice/.../cri-containerd-<id>.scope (systemd driver).
#
# Why both this AND run-k3d.sh? They exercise DIFFERENT cgroup shapes:
#   * k3d   = nested  → chain depth 3 (docker → kubernetes → containerd)
#   * k0s   = native  → chain depth 2 (kubernetes → containerd)
# The leaf-wins rule has to work for both; the chain shape proves we
# didn't accidentally invent a docker wrapper that isn't there.
#
# Requires: k0s installed as systemd service (provisioner does this),
# kubectl in PATH, CAP_SYS_ADMIN for the probe.
# Exit:  77 = SKIP (k0s not installed / service won't start)
#        70 = harness error
#        0/1/2 = probe exit code
#
# WARNING: k0s bundles kube-router which inserts persistent FORWARD/INPUT
# chain rules and zombie veth devices on cni-podman0/docker0 that survive
# `systemctl stop k0scontroller`. If this runner has run on a given boot,
# subsequent `attribution_podman` runs may fail with "ss did not observe
# an inbound flow" — the container's packets never reach the host bridge.
# CTest orders attribution_podman (24) BEFORE attribution_k8s (26) so a
# clean cold-boot run is fine; if you re-run podman after k0s started,
# `vagrant reload --no-provision` first. See AGENTS.md §6.5a.
set -euo pipefail

readonly POD_NAME="qiftop-attr-k8s-pod"
readonly LISTEN_PORT=18083
readonly PROBE_BIN="${QIFTOP_PROBE_BIN:-}"
readonly KUBECONFIG_TMP="$(mktemp --suffix=.kubeconfig)"

if [[ -z "$PROBE_BIN" || ! -x "$PROBE_BIN" ]]; then
    echo "harness: QIFTOP_PROBE_BIN unset or not executable ('$PROBE_BIN')" >&2
    exit 70
fi
for tool in k0s kubectl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "harness: $tool not in PATH; SKIPPING" >&2
        exit 77
    fi
done

# k0s requires root for `k0s kubeconfig admin` and for managing the
# embedded containerd's namespace. We're invoked under sudo from ctest
# already (the probe needs CAP_SYS_ADMIN), so this is just defensive.
if [[ "$EUID" -ne 0 ]]; then
    echo "harness: must run as root (re-invoke via sudo)" >&2
    exit 70
fi

LISTENER_LOG=$(mktemp)
PROBE_TMP=$(mktemp)
STARTED_K0S=0
cleanup() {
    echo "harness: cleanup" >&2
    if [[ -n "${POD_CREATED:-}" ]]; then
        kubectl --kubeconfig "$KUBECONFIG_TMP" delete pod "$POD_NAME" \
            --grace-period=0 --force >/dev/null 2>&1 || true
    fi
    [[ -n "${LISTENER_PID:-}" ]] && kill -9 "$LISTENER_PID" 2>/dev/null || true
    # Leave k0s running if it was already up before us — only stop it if
    # we were the ones who started it.
    if [[ "$STARTED_K0S" -eq 1 ]]; then
        systemctl stop k0scontroller >/dev/null 2>&1 || true
    fi
    rm -f "$LISTENER_LOG" "$PROBE_TMP" "$KUBECONFIG_TMP"
}
trap cleanup EXIT

echo "harness: ensuring k0s controller is running"
if ! systemctl is-active --quiet k0scontroller; then
    if ! systemctl start k0scontroller; then
        echo "harness: failed to start k0scontroller; SKIPPING" >&2
        exit 77
    fi
    STARTED_K0S=1
    echo "harness: started k0s (cold-start may take ~30s)"
fi

# Wait for the API server to be reachable. k0s creates its admin
# kubeconfig at /var/lib/k0s/pki/admin.conf once the API is up.
echo "harness: waiting for k0s API server"
for _ in $(seq 1 60); do
    if [[ -f /var/lib/k0s/pki/admin.conf ]] && \
       k0s kubeconfig admin > "$KUBECONFIG_TMP" 2>/dev/null && \
       kubectl --kubeconfig "$KUBECONFIG_TMP" get --raw=/readyz >/dev/null 2>&1; then
        break
    fi
    sleep 1
done
if ! kubectl --kubeconfig "$KUBECONFIG_TMP" get --raw=/readyz >/dev/null 2>&1; then
    echo "harness: k0s API server never became ready" >&2
    exit 70
fi
export KUBECONFIG="$KUBECONFIG_TMP"

# Wait for the default ServiceAccount — even on naked k8s the
# controller-manager creates it asynchronously after the API comes up,
# and the first pod create gets "serviceaccount default not found" if
# we don't.
echo "harness: waiting for default ServiceAccount"
for _ in $(seq 1 60); do
    if kubectl get sa default >/dev/null 2>&1; then break; fi
    sleep 1
done
if ! kubectl get sa default >/dev/null 2>&1; then
    echo "harness: default ServiceAccount never appeared" >&2
    exit 70
fi

# Wait for the node to register and become Ready. On naked k8s the
# node IS the host, but the kubelet takes ~30-60s past API-up to
# register itself — and `kubectl wait` returns "no matching
# resources" if it runs before that. So pre-poll for any node to
# exist, THEN wait on Ready.
echo "harness: waiting for node to register"
for _ in $(seq 1 120); do
    if [[ -n "$(kubectl get nodes -o name 2>/dev/null)" ]]; then break; fi
    sleep 1
done
echo "harness: waiting for node Ready"
if ! kubectl wait --for=condition=Ready nodes --all --timeout=90s >/dev/null; then
    echo "harness: node never became Ready" >&2
    exit 70
fi

# Host IP the pod can reach. With k0s the pod runs in the host's
# network namespace ONLY if hostNetwork=true; by default it gets a
# cluster IP and reaches the host via the CNI's bridge. Resolve the
# default-route source IP — that's what the pod will dial.
HOST_IP="$(ip -4 route get 1.1.1.1 2>/dev/null \
    | awk '/src/ {for (i=1;i<=NF;i++) if ($i=="src") print $(i+1); exit}')"
if [[ -z "$HOST_IP" ]]; then
    # CI runners can have a weird default route; fall back to the first
    # non-loopback global IPv4.
    HOST_IP="$(ip -4 -o addr show scope global \
        | awk '{print $4}' | cut -d/ -f1 | head -1)"
fi
if [[ -z "$HOST_IP" ]]; then
    echo "harness: could not determine a usable host IP" >&2
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

# Launch a pod that holds a long-lived TCP outbound to the host.
echo "harness: launching pod $POD_NAME"
kubectl run "$POD_NAME" \
    --image=alpine:3.20 \
    --restart=Never \
    --image-pull-policy=IfNotPresent \
    --command -- sh -c \
    "while true; do nc -w 30 ${HOST_IP} ${LISTEN_PORT} </dev/zero >/dev/null; sleep 0.1; done" \
    >/dev/null
POD_CREATED=1
kubectl wait --for=condition=Ready "pod/${POD_NAME}" --timeout=120s >/dev/null

# Capture the pod's container ID. k0s (embedded containerd) reports IDs
# prefixed by "containerd://<64hex>" in the status, same as k3d.
RAW_CID="$(kubectl get pod "$POD_NAME" -o jsonpath='{.status.containerStatuses[0].containerID}')"
CID="${RAW_CID#*://}"
CID_SHORT="${CID:0:12}"
echo "harness: pod container CID = ${CID_SHORT}"

# Ask the pod itself for its outbound 4-tuple — same trick as k3d, but
# here the netns hop is single (pod-netns → host) so the tuple we get
# IS the tuple the host's conntrack will see. We still go through the
# pod because NetnsScanner is what we're exercising — it must walk
# into the pod's netns to find the socket.
POD_TUPLE=""
for _ in $(seq 1 30); do
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

# Expectations:
#   * runtime ∈ {containerd, kubernetes} — same as k3d, leaf may
#     classify either way depending on cgroup driver.
#   * chain depth EXACTLY 2 (kubernetes → containerd). This is the
#     distinguishing assertion vs. k3d's depth-3 chain — proves
#     classifyPathChain doesn't hallucinate a phantom docker wrapper
#     when there isn't one.
#   * chain MUST NOT contain a 'docker' entry — same point, phrased
#     as a negative.
# We don't have a "max depth" flag on the probe, but a missing-docker
# assertion plus min-depth-2 pin both ends. (Hard --expect-chain-depth-exact
# would be a future probe feature; min-depth + missing-runtime suffices.)
set +e
"$PROBE_BIN" --json \
    --proto tcp \
    --local  "${POD_TUPLE}" \
    --remote "${LOCAL_TUPLE}" \
    --expect-container-runtime containerd,kubernetes \
    --expect-chain-min-depth 2 \
    --expect-chain-contains containerd,kubernetes \
    --timeout-ms 15000 | tee "$PROBE_TMP"
PROBE_RC=$?
set -e

# Negative assertion: chain MUST NOT contain a docker wrapper. The
# probe doesn't have a --expect-chain-excludes flag (yet); grep the
# JSON instead. This is a cheap post-condition that catches the
# regression we'd most fear: classifyPathChain accidentally promoting
# a systemd wrapper segment or some kernel-side scope to 'docker'.
if [[ "$PROBE_RC" -eq 0 ]] && grep -q '"runtime":"docker"' "$PROBE_TMP"; then
    echo "harness: FAIL — chain unexpectedly contains a docker wrapper:" >&2
    cat "$PROBE_TMP" >&2
    exit 1
fi

exit "$PROBE_RC"
