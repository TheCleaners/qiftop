#!/usr/bin/env python3
"""bpf-eval scorer: score a capture path's snapshots against the generator's
authoritative ground truth.

Path-agnostic by design — it takes any collector's NDJSON in the common schema
(see conntrack_collect.cpp) so the SAME scorer grades conntrack, pcap, and eBPF
collectors with no per-path special-casing (DESIGN.md §5).

Usage:
  score.py --truth groundtruth.ndjson --snapshots conntrack.ndjson \
           [--poll-ms 1000] [--json out.json]

Ground-truth record (one per flow), from gen.cpp:
  flow_id, proto, family, local_ip, local_port, remote_ip, remote_port,
  pid, starttime, initiator, bytes_l2r, bytes_r2l, t_open_ms, t_close_ms
Snapshot record (one per flow per snapshot), from a collector:
  ts_ms, proto, local_ip, local_port, remote_ip, remote_port, rx, tx,
  dir, reason, pid
"""
import argparse
import json
import sys
from collections import defaultdict


def load_ndjson(path):
    rows = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
    except FileNotFoundError:
        print(f"score: {path} not found — skipping", file=sys.stderr)
    return rows


def flow_key(r):
    # 5-tuple; the ephemeral local_port makes each generated flow unique.
    return (r["proto"], r["local_ip"], int(r["local_port"]),
            r["remote_ip"], int(r["remote_port"]))


def pct(n, d):
    return (100.0 * n / d) if d else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--truth", required=True)
    ap.add_argument("--snapshots", required=True)
    ap.add_argument("--poll-ms", type=int, default=1000)
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    truth = load_ndjson(args.truth)
    snaps = load_ndjson(args.snapshots)

    if not truth:
        print("score: no ground-truth flows — nothing to score", file=sys.stderr)
        return 0

    # Index snapshot observations per flow key: first-seen ts, last captured
    # dir/pid/rx/tx (use the max bytes seen — counters are monotonic).
    seen = {}
    for s in snaps:
        k = flow_key(s)
        e = seen.get(k)
        if e is None:
            e = {"first_ts": s["ts_ms"], "dir": s["dir"], "pid": s["pid"],
                 "rx": s["rx"], "tx": s["tx"]}
            seen[k] = e
        else:
            e["first_ts"] = min(e["first_ts"], s["ts_ms"])
            e["rx"] = max(e["rx"], s["rx"])
            e["tx"] = max(e["tx"], s["tx"])
            # Prefer a non-unknown direction / non-zero pid if it ever appears.
            if e["dir"] == "unknown" and s["dir"] != "unknown":
                e["dir"] = s["dir"]
            if e["pid"] == 0 and s["pid"] != 0:
                e["pid"] = s["pid"]

    # Buckets by lifetime relative to the poll interval.
    def bucket(t):
        return "subpoll" if (t["t_close_ms"] - t["t_open_ms"]) < args.poll_ms else "multipoll"

    n = defaultdict(int)          # totals per bucket
    covered = defaultdict(int)    # matched per bucket
    dir_correct = dir_wrong = dir_unknown = 0
    attr_correct = attr_missed = attr_wrong = 0
    byte_num = byte_den = 0
    latencies = []

    for t in truth:
        b = bucket(t)
        n[b] += 1
        n["all"] += 1
        k = flow_key(t)
        e = seen.get(k)
        if not e:
            continue
        covered[b] += 1
        covered["all"] += 1

        # Direction: ground-truth initiator is always "outbound" (worker dials).
        if e["dir"] == "unknown":
            dir_unknown += 1
        elif e["dir"] == t["initiator"]:
            dir_correct += 1
        else:
            dir_wrong += 1

        # Attribution: captured pid vs ground-truth pid.
        if e["pid"] == 0:
            attr_missed += 1
        elif e["pid"] == t["pid"]:
            attr_correct += 1
        else:
            attr_wrong += 1

        # Byte coverage (conditional on being seen). GT bytes_l2r = local->remote
        # = captured tx; bytes_r2l = remote->local = captured rx.
        byte_den += t["bytes_l2r"] + t["bytes_r2l"]
        byte_num += min(e["tx"], t["bytes_l2r"]) + min(e["rx"], t["bytes_r2l"])

        # Capture latency: first appearance minus open.
        lat = e["first_ts"] - t["t_open_ms"]
        if lat >= 0:
            latencies.append(lat)

    latencies.sort()

    def p(q):
        return latencies[min(len(latencies) - 1, int(q * len(latencies)))] if latencies else None

    matched = covered["all"]
    metrics = {
        "ground_truth_flows": n["all"],
        "flow_coverage_pct": round(pct(covered["all"], n["all"]), 2),
        "flow_coverage_subpoll_pct": round(pct(covered["subpoll"], n["subpoll"]), 2),
        "flow_coverage_multipoll_pct": round(pct(covered["multipoll"], n["multipoll"]), 2),
        "subpoll_flows": n["subpoll"],
        "multipoll_flows": n["multipoll"],
        "byte_coverage_conditional_pct": round(pct(byte_num, byte_den), 2),
        "direction_correct_pct": round(pct(dir_correct, matched), 2),
        "direction_wrong_pct": round(pct(dir_wrong, matched), 2),
        "direction_unknown_pct": round(pct(dir_unknown, matched), 2),
        "attribution_correct_pct": round(pct(attr_correct, matched), 2),
        "attribution_missed_pct": round(pct(attr_missed, matched), 2),
        "attribution_wrong_pct": round(pct(attr_wrong, matched), 2),
        "capture_latency_ms_p50": p(0.50),
        "capture_latency_ms_p95": p(0.95),
    }

    w = sys.stdout.write
    w("\n=== bpf-eval score: %s vs %s ===\n" % (args.snapshots, args.truth))
    w("ground-truth flows : %d (subpoll=%d, multipoll=%d)\n"
      % (n["all"], n["subpoll"], n["multipoll"]))
    w("flow coverage      : %.1f%%  (subpoll %.1f%% | multipoll %.1f%%)\n"
      % (metrics["flow_coverage_pct"], metrics["flow_coverage_subpoll_pct"],
         metrics["flow_coverage_multipoll_pct"]))
    w("byte coverage      : %.1f%% (of seen flows)\n" % metrics["byte_coverage_conditional_pct"])
    w("direction          : %.1f%% correct | %.1f%% wrong | %.1f%% unknown\n"
      % (metrics["direction_correct_pct"], metrics["direction_wrong_pct"],
         metrics["direction_unknown_pct"]))
    w("attribution        : %.1f%% correct | %.1f%% missed | %.1f%% wrong\n"
      % (metrics["attribution_correct_pct"], metrics["attribution_missed_pct"],
         metrics["attribution_wrong_pct"]))
    w("capture latency    : p50=%s ms  p95=%s ms\n"
      % (metrics["capture_latency_ms_p50"], metrics["capture_latency_ms_p95"]))
    w("\nNOTE: missed flows (esp. subpoll) are the conntrack-vs-capture story; "
      "direction 'unknown' on a host-only run usually means the remote wasn't "
      "made non-local (use the peer-netns orchestrator).\n\n")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(metrics, f, indent=2)
        print("score: wrote %s" % args.json, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
