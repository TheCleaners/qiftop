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
command -v nft >/dev/null 2>&1 || skip "nftables 'nft' not found (needed to activate conntrack tracking)"
[ -x "$BIN_DIR/qiftop-bpfeval-sink" ] || skip "sink binary missing (build with -DQIFTOP_BUILD_BPF_EVAL=ON)"
[ -x "$BIN_DIR/qiftop-bpfeval-gen" ]  || skip "generator binary missing"
[ -x "$BIN_DIR/qiftop-bpfeval-conntrack" ] || skip "conntrack collector missing (Linux backend needed)"
modprobe nf_conntrack 2>/dev/null || true
[ -e /proc/net/nf_conntrack ] || [ -d /proc/sys/net/netfilter ] || skip "conntrack not available in this kernel"

SINK_PID=""; COLL_PID=""; BPF_PID=""
cleanup() {
    [ -n "$SINK_PID" ] && kill "$SINK_PID" 2>/dev/null || true
    [ -n "$COLL_PID" ] && kill "$COLL_PID" 2>/dev/null || true
    [ -n "$BPF_PID" ]  && kill "$BPF_PID" 2>/dev/null || true
    nft delete table inet qiftop_eval 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$VETH_H" 2>/dev/null || true
    echo "cleanup done; artifacts in $WORK"
}
trap cleanup EXIT INT TERM

# --- activate conntrack tracking ------------------------------------------
# The kernel only tracks a family once SOME nftables/iptables rule references
# `ct`. Without this, conntrack stays empty and the collector sees nothing —
# exactly the gotcha the agent's systemd unit works around with its inert
# `inet qiftop` shim. We install an equivalent, in our own table so we never
# clash with a running agent. Also enable byte accounting up front.
nft delete table inet qiftop_eval 2>/dev/null || true
nft -f - <<'NFT' 2>/dev/null || skip "could not load nftables conntrack shim"
table inet qiftop_eval {
    chain track {
        type filter hook prerouting priority -200; policy accept;
        ct state new counter
    }
}
NFT
sysctl -wq net.netfilter.nf_conntrack_acct=1 2>/dev/null || true

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

# --- eBPF socket-birth collector (Phase 2, optional) ----------------------
# Captures pid + direction at connect() time, before short-lived processes
# exit — the gap conntrack+sock_diag can't close. bpftrace is a runtime tool,
# so this stays zero-build-deps; skip cleanly if it's not installed or the
# kernel lacks the tracepoint.
BIRTH_BT="$BIN_DIR/birth.bt"
if command -v bpftrace >/dev/null 2>&1 && [ -f "$BIRTH_BT" ]; then
    bpftrace "$BIRTH_BT" >"$WORK/ebpf_birth.ndjson" 2>"$WORK/bpftrace.log" &
    BPF_PID=$!
    sleep 1.0  # bpftrace needs a moment to attach the probes
    echo "bpf-eval: eBPF-birth collector attached (pid $BPF_PID)"
else
    echo "bpf-eval: bpftrace not available — skipping eBPF-birth collector (conntrack only)"
fi
sleep 0.5  # let the first conntrack dump establish a baseline

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
  S2U) # UDP churn — same as S2 but connected-UDP flows (tests UDP birth)
      "$BIN_DIR/qiftop-bpfeval-gen" "${GEN_COMMON[@]}" \
          --mode churn --proto udp --workers 8 --bytes 2048 --lifetime-ms 200 --duration-ms 6000 ;;
  *)  skip "unknown scenario '$SCENARIO' (use S1|S2|S3|S2U)" ;;
esac

# Let the collector capture a couple more polls after the last flow.
sleep 2
wait "$COLL_PID" 2>/dev/null || true
[ -n "$BPF_PID" ] && kill "$BPF_PID" 2>/dev/null || true
sleep 0.3  # let bpftrace flush its buffer

# --- assemble ground truth + score ---------------------------------------
cat "$WORK"/gt.*.ndjson > "$WORK/groundtruth.ndjson" 2>/dev/null || true
GT_N=$(wc -l < "$WORK/groundtruth.ndjson" 2>/dev/null || echo 0)
CT_N=$(wc -l < "$WORK/conntrack.ndjson" 2>/dev/null || echo 0)
echo "bpf-eval: ground-truth flows=$GT_N  conntrack flow-observations=$CT_N"

echo "==================== conntrack capture path ===================="
python3 "$BIN_DIR/score.py" --truth "$WORK/groundtruth.ndjson" \
    --snapshots "$WORK/conntrack.ndjson" --poll-ms 1000 \
    --json "$WORK/metrics.$SCENARIO.conntrack.json"

if [ -s "$WORK/ebpf_birth.ndjson" ]; then
    EB_N=$(wc -l < "$WORK/ebpf_birth.ndjson")
    echo "bpf-eval: eBPF-birth events=$EB_N"
    echo "==================== eBPF socket-birth path ===================="
    echo "(byte coverage is 0 by design — birth has no bytes; the hybrid gets"
    echo " those from conntrack. Watch ATTRIBUTION + LATENCY vs conntrack above.)"
    python3 "$BIN_DIR/score.py" --truth "$WORK/groundtruth.ndjson" \
        --snapshots "$WORK/ebpf_birth.ndjson" --poll-ms 1000 \
        --json "$WORK/metrics.$SCENARIO.ebpf_birth.json"
fi

echo "bpf-eval: metrics + raw NDJSON in $WORK"
