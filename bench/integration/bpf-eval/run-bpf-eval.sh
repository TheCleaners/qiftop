#!/usr/bin/env bash
# bpf-eval orchestrator — Phase 0 (conntrack baseline).
#
# Brings up a "remote peer" network namespace (so generated flows have a
# genuinely NON-LOCAL remote — loopback would make direction ambiguous), runs
# the sink in it, runs the REAL ConntrackMonitor collector + the ground-truth
# generator in the host netns, then scores capture against ground truth.
#
# SKIP-SAFE: if a prerequisite is missing (not root, no `ip`, binaries not
# built, no conntrack), it prints a SKIP line and exits 0 — never fails a CI or
# a dev machine that can't run it. This is VM-only by intent.
set -u

SCENARIO="${1:-S2}"
BIN_DIR="${BIN_DIR:-$(cd "$(dirname "$0")" && pwd)}"
WORK="$(mktemp -d /tmp/bpfeval.XXXXXX)"
NS="qifpeer"
VETH_H="vqif0"
VETH_P="vqif1"
HOST_IP="10.77.0.1"
PEER_IP="10.77.0.2"
PORT=18080
DURATION_MS=8000

skip() { echo "SKIP: $*"; exit 0; }

# --- prerequisites --------------------------------------------------------
[ "$(id -u)" -eq 0 ] || skip "needs root (conntrack + netns). Run on the VM as root."
command -v ip >/dev/null 2>&1 || skip "iproute2 'ip' not found"
[ -x "$BIN_DIR/qiftop-bpfeval-sink" ] || skip "sink binary missing (build with -DQIFTOP_BUILD_BPF_EVAL=ON)"
[ -x "$BIN_DIR/qiftop-bpfeval-gen" ]  || skip "generator binary missing"
[ -x "$BIN_DIR/qiftop-bpfeval-conntrack" ] || skip "conntrack collector missing (Linux backend needed)"
modprobe nf_conntrack 2>/dev/null || true
[ -e /proc/net/nf_conntrack ] || [ -d /proc/sys/net/netfilter ] || skip "conntrack not available in this kernel"

SINK_PID=""; COLL_PID=""
cleanup() {
    [ -n "$SINK_PID" ] && kill "$SINK_PID" 2>/dev/null || true
    [ -n "$COLL_PID" ] && kill "$COLL_PID" 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$VETH_H" 2>/dev/null || true
    echo "cleanup done; artifacts in $WORK"
}
trap cleanup EXIT INT TERM

# --- peer netns + veth ----------------------------------------------------
ip netns del "$NS" 2>/dev/null || true
ip link del "$VETH_H" 2>/dev/null || true
ip netns add "$NS"
ip link add "$VETH_H" type veth peer name "$VETH_P"
ip link set "$VETH_P" netns "$NS"
ip addr add "$HOST_IP/24" dev "$VETH_H"
ip link set "$VETH_H" up
ip -n "$NS" addr add "$PEER_IP/24" dev "$VETH_P"
ip -n "$NS" link set "$VETH_P" up
ip -n "$NS" link set lo up

echo "bpf-eval: scenario=$SCENARIO host=$HOST_IP peer=$PEER_IP port=$PORT work=$WORK"

# --- sink in peer netns ---------------------------------------------------
ip netns exec "$NS" "$BIN_DIR/qiftop-bpfeval-sink" --port "$PORT" --duration "$(( DURATION_MS / 1000 + 5 ))" &
SINK_PID=$!
sleep 0.3

# --- conntrack collector in host netns ------------------------------------
"$BIN_DIR/qiftop-bpfeval-conntrack" --duration-ms "$DURATION_MS" --poll-ms 1000 \
    --out "$WORK/conntrack.ndjson" &
COLL_PID=$!
sleep 0.5  # let the first dump establish a baseline

# --- generator (host netns → peer) ----------------------------------------
GEN_COMMON=(--sink-host "$PEER_IP" --sink-port "$PORT" --out-dir "$WORK")
case "$SCENARIO" in
  S1) # steady many concurrent flows
      "$BIN_DIR/qiftop-bpfeval-gen" "${GEN_COMMON[@]}" \
          --mode concurrent --workers 4 --flows-per-worker 250 --bytes 8192 --hold-ms 4000 ;;
  S2) # churn: many sub-poll short flows (the conntrack-miss scenario)
      "$BIN_DIR/qiftop-bpfeval-gen" "${GEN_COMMON[@]}" \
          --mode churn --workers 8 --bytes 4096 --lifetime-ms 200 --duration-ms 6000 ;;
  S3) # many PIDs + reuse
      "$BIN_DIR/qiftop-bpfeval-gen" "${GEN_COMMON[@]}" \
          --mode churn --workers 200 --bytes 2048 --lifetime-ms 100 --duration-ms 6000 ;;
  *)  skip "unknown scenario '$SCENARIO' (use S1|S2|S3)" ;;
esac

# Let the collector capture a couple more polls after the last flow.
sleep 2
wait "$COLL_PID" 2>/dev/null || true

# --- assemble ground truth + score ---------------------------------------
cat "$WORK"/gt.*.ndjson > "$WORK/groundtruth.ndjson" 2>/dev/null || true
GT_N=$(wc -l < "$WORK/groundtruth.ndjson" 2>/dev/null || echo 0)
CT_N=$(wc -l < "$WORK/conntrack.ndjson" 2>/dev/null || echo 0)
echo "bpf-eval: ground-truth flows=$GT_N  conntrack flow-observations=$CT_N"

python3 "$BIN_DIR/score.py" --truth "$WORK/groundtruth.ndjson" \
    --snapshots "$WORK/conntrack.ndjson" --poll-ms 1000 \
    --json "$WORK/metrics.$SCENARIO.json"

echo "bpf-eval: metrics + raw NDJSON in $WORK"
