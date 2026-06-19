#!/usr/bin/env bash
# bpf-eval cost gate — measure the CPU cost of the eBPF socket-birth path under
# high connect-rate load (the gate the idle-VM scoring runs can't answer).
#
# Two numbers, both honest:
#   1) PURE kernel eBPF cost per connect — via kernel.bpf_stats_enabled +
#      `bpftool prog show` run_time_ns / run_cnt. This EXCLUDES bpftrace's
#      per-event userspace printf, so it reflects what a production ring-buffer
#      implementation would actually pay in the kernel. This is the number that
#      matters for the build decision.
#   2) End-to-end system-CPU delta (conntrack vs conntrack+eBPF) over identical
#      load — INCLUDES bpftrace's heavy printf, so it's a conservative UPPER
#      bound (production would be cheaper).
#
# SKIP-SAFE: needs root + nft + the binaries + bpftrace; bpftool is optional
# (number 1 is skipped without it, number 2 still works).
set -u

BIN_DIR="${BIN_DIR:-$(cd "$(dirname "$0")" && pwd)}"
WORK="$(mktemp -d /tmp/bpfcost.XXXXXX)"
NS="qifpeer"; VETH_H="vqif0"; VETH_P="vqif1"
HOST_IP="10.77.0.1"; PEER_IP="10.77.0.2"; PORT=18080
WORKERS="${WORKERS:-16}"; LOAD_MS="${LOAD_MS:-8000}"

skip() { echo "SKIP: $*"; exit 0; }
[ "$(id -u)" -eq 0 ] || skip "needs root"
command -v ip >/dev/null 2>&1 || skip "no ip"
command -v nft >/dev/null 2>&1 || skip "no nft"
command -v bpftrace >/dev/null 2>&1 || skip "no bpftrace"
[ -x "$BIN_DIR/qiftop-bpfeval-gen" ] || skip "gen missing"
[ -x "$BIN_DIR/qiftop-bpfeval-sink" ] || skip "sink missing"
[ -x "$BIN_DIR/qiftop-bpfeval-conntrack" ] || skip "conntrack collector missing"
HAVE_BPFTOOL=0; command -v bpftool >/dev/null 2>&1 && HAVE_BPFTOOL=1

CLK=$(getconf CLK_TCK)

SINK_PID=""; COLL_PID=""; BPF_PID=""
cleanup() {
    [ -n "$SINK_PID" ] && kill "$SINK_PID" 2>/dev/null || true
    [ -n "$COLL_PID" ] && kill "$COLL_PID" 2>/dev/null || true
    [ -n "$BPF_PID" ]  && kill "$BPF_PID"  2>/dev/null || true
    nft delete table inet qiftop_eval 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$VETH_H" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Busy CPU jiffies (everything except idle+iowait) summed across all CPUs.
cpu_busy() {
    awk '/^cpu /{ idle=$5+$6; total=0; for(i=2;i<=NF;i++) total+=$i; print total-idle }' /proc/stat
}

setup_net() {
    nft delete table inet qiftop_eval 2>/dev/null || true
    ip netns del "$NS" 2>/dev/null || true
    ip link del "$VETH_H" 2>/dev/null || true
    nft -f - <<'NFT' 2>/dev/null || skip "nft shim failed"
table inet qiftop_eval {
    chain track {
        type filter hook prerouting priority -200; policy accept;
        ct state new counter
    }
}
NFT
    sysctl -wq net.netfilter.nf_conntrack_acct=1 2>/dev/null || true
    ip netns add "$NS"
    ip link add "$VETH_H" type veth peer name "$VETH_P"
    ip link set "$VETH_P" netns "$NS"
    ip addr add "$HOST_IP/24" dev "$VETH_H"; ip link set "$VETH_H" up
    ip -n "$NS" addr add "$PEER_IP/24" dev "$VETH_P"
    ip -n "$NS" link set "$VETH_P" up; ip -n "$NS" link set lo up
}

# Run one load burst in a given config; echo "connects busy_jiffies".
# $1 = label, $2 = with_conntrack(0/1), $3 = with_ebpf(0/1)
run_config() {
    local label="$1" with_ct="$2" with_bpf="$3"
    local cdir; cdir="$WORK/$label"; mkdir -p "$cdir"
    SINK_PID=""; COLL_PID=""; BPF_PID=""
    ip netns exec "$NS" "$BIN_DIR/qiftop-bpfeval-sink" --port "$PORT" --duration "$(( LOAD_MS/1000 + 4 ))" & SINK_PID=$!
    sleep 0.3
    if [ "$with_ct" = 1 ]; then
        "$BIN_DIR/qiftop-bpfeval-conntrack" --duration-ms "$(( LOAD_MS + 2000 ))" --poll-ms 1000 --out "$cdir/ct.ndjson" & COLL_PID=$!
    fi
    if [ "$with_bpf" = 1 ]; then
        bpftrace "$BIN_DIR/birth.bt" >"$cdir/birth.ndjson" 2>/dev/null & BPF_PID=$!
        sleep 1.0
    fi
    local b0 b1
    b0=$(cpu_busy)
    "$BIN_DIR/qiftop-bpfeval-gen" --sink-host "$PEER_IP" --sink-port "$PORT" --out-dir "$cdir" \
        --mode churn --workers "$WORKERS" --bytes 2048 --lifetime-ms 0 --duration-ms "$LOAD_MS" >/dev/null 2>&1
    b1=$(cpu_busy)
    # Capture eBPF program stats WHILE the probes are still attached (bpftool
    # sees nothing once bpftrace exits and unloads them).
    if [ "$with_bpf" = 1 ] && [ "$HAVE_BPFTOOL" = 1 ]; then
        bpftool prog show -j >"$cdir/bpftool.json" 2>/dev/null || true
    fi
    [ -n "$BPF_PID" ]  && kill "$BPF_PID"  2>/dev/null || true
    [ -n "$COLL_PID" ] && kill "$COLL_PID" 2>/dev/null || true
    [ -n "$SINK_PID" ] && kill "$SINK_PID" 2>/dev/null || true
    sleep 0.3
    local connects; connects=$(cat "$cdir"/gt.*.ndjson 2>/dev/null | wc -l)
    echo "$connects $(( b1 - b0 ))"
}

setup_net
echo "bpf-cost: workers=$WORKERS load=${LOAD_MS}ms CLK_TCK=$CLK  (each config = one churn burst)"

# Warm caches with a throwaway run.
run_config warmup 1 0 >/dev/null

read CN_BASE J_BASE < <(run_config baseline 0 0)
read CN_CT   J_CT   < <(run_config conntrack 1 0)

# eBPF config with bpf_stats for the pure-kernel number.
sysctl -wq kernel.bpf_stats_enabled=1 2>/dev/null || true
read CN_BPF  J_BPF  < <(run_config conntrack_ebpf 1 1)
RUN_NS=0; RUN_CNT=0
if [ "$HAVE_BPFTOOL" = 1 ] && [ -s "$WORK/conntrack_ebpf/bpftool.json" ]; then
    # Sum run_time_ns / run_cnt across the kprobe/kretprobe progs bpftrace loaded.
    read RUN_NS RUN_CNT < <(python3 -c 'import json,sys
d=json.load(open(sys.argv[1])); ns=cnt=0
for p in d:
    if p.get("type") in ("kprobe","tracing","raw_tracepoint","perf_event") and "run_time_ns" in p:
        ns+=p.get("run_time_ns",0); cnt+=p.get("run_cnt",0)
print(ns, cnt)' "$WORK/conntrack_ebpf/bpftool.json" 2>/dev/null || echo "0 0")
fi
sysctl -wq kernel.bpf_stats_enabled=0 2>/dev/null || true

js2cpu() { awk -v j="$1" -v c="$CLK" 'BEGIN{printf "%.3f", j/c}'; }  # jiffies→CPU-seconds

echo ""
echo "=================== bpf-eval CPU cost ==================="
printf "config            connects    busy(CPU-s)\n"
printf "baseline          %-10s  %s\n" "$CN_BASE" "$(js2cpu "$J_BASE")"
printf "conntrack         %-10s  %s\n" "$CN_CT"   "$(js2cpu "$J_CT")"
printf "conntrack+eBPF    %-10s  %s\n" "$CN_BPF"  "$(js2cpu "$J_BPF")"
echo ""
# Marginal eBPF end-to-end (incl. bpftrace printf — conservative upper bound).
awk -v jb="$J_CT" -v je="$J_BPF" -v cn="$CN_BPF" -v c="$CLK" -v w="$LOAD_MS" 'BEGIN{
    d=(je-jb)/c;                       # extra CPU-seconds from the eBPF path
    if(cn>0) printf "eBPF end-to-end (incl. bpftrace printf): +%.3f CPU-s over %d connects = %.2f us/connect; %.1f%% of one core\n", d, cn, d*1e6/cn, 100*d/(w/1000.0);
}'
if [ "$RUN_CNT" -gt 0 ] 2>/dev/null; then
    awk -v ns="$RUN_NS" -v cnt="$RUN_CNT" -v w="$LOAD_MS" 'BEGIN{
        printf "eBPF PURE kernel (bpftool run_time_ns, excludes printf): %d events, %.0f ns/event, %.4f CPU-s total, %.3f%% of one core during the burst\n", cnt, ns/cnt, ns/1e9, 100*(ns/1e9)/(w/1000.0);
    }'
else
    echo "eBPF PURE kernel cost: bpftool number unavailable"
fi
echo ""
echo "NOTE: the busy(CPU-s) deltas are dominated by the load itself (the gen+sink"
echo "saturate all CPUs at this connect rate), so the end-to-end delta is noisy by"
echo "design. The bpftool run_time_ns line is the load-INDEPENDENT, production-"
echo "relevant number — it's the eBPF program's own kernel CPU per connect event."
echo "artifacts: $WORK"
