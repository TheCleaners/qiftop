#!/usr/bin/env bash
#
# run-systemd-dbus.sh — Tier-2 attribution test against the REAL systemd
# agent, queried over DBus.
#
# WHY THIS EXISTS (vs. the probe-based runners):
#   The other attribution runners drive qiftop-attribution-probe, which
#   builds the resolver chain IN-PROCESS — no systemd unit, no sandbox.
#   That means they cannot catch regressions in the agent's *execution
#   environment* (systemd hardening directives, capabilities, the DBus
#   policy). A real example this guards against: RestrictNamespaces=yes
#   silently seccomp-blocked NetnsScanner's setns(CLONE_NEWNET), so the
#   systemd agent attributed every container flow as host — while the
#   probe (run directly as root, no sandbox) passed. See AGENTS.md §8a.
#
#   This runner instead:
#     1. installs the freshly-built qiftop-agent component (so the unit
#        under test is OURS, sandbox directives and all),
#     2. starts it via systemd,
#     3. runs a docker container that opens ONE long-lived outbound TCP
#        flow to an external host (so it is masqueraded => conntracked,
#        AND the socket stays alive so the resolver can attribute it),
#     4. queries the agent over DBus (sudo busctl GetConnections) and
#        asserts the flow attributes to runtime=docker + the right CID.
#
# FLOW GENERATOR NOTE (learned the hard way — see checkpoint 024):
#   Container attribution needs a flow that is BOTH (a) conntracked and
#   (b) backed by a LIVE socket at query time. That rules out:
#     * container -> docker0/host-local IP   (not conntracked: local delivery)
#     * container -> container same bridge   (L2-switched: not conntracked)
#     * busybox `wget https://...`           (no TLS in alpine busybox => no socket)
#     * `nc host port` in a reconnect loop   (PID churns => PID-reuse guard rejects)
#     * detached `nc` with closed stdin      (EOFs and exits immediately)
#   The reliable recipe is a SINGLE long-lived process holding ONE socket
#   to an external host that keeps the connection open. We use
#   `sleep <big> | nc <host> 22`: the sleep keeps nc's stdin open so it
#   never EOFs, and an SSH server holds the pre-auth connection for its
#   LoginGraceTime (~120s) — far longer than this test runs.
#
# DESTRUCTIVE: installs files under /usr + /lib/systemd, (re)starts a
# system service. Intended ONLY for the disposable Vagrant VM
# (tests/integration/vagrant) or a CI throwaway runner — NEVER a dev
# workstation. It refuses to run unless it can use sudo non-interactively.
#
# Requires (else SKIP 77):
#   - root / passwordless sudo
#   - systemd (systemctl) + busctl
#   - docker reachable
#   - outbound TCP to QIFTOP_PROBE_TARGET_HOST:QIFTOP_PROBE_TARGET_PORT
#     (default github.com:22)
#   - QIFTOP_BUILD_DIR pointing at a configured+built CMake build dir
#     with the qiftop-agent target built (set by the ctest entry).
#
# Exit: 0=ok, 1=mismatch (flow present but NOT attributed = the bug we
#       guard), 70=harness error, 77=skip.

set -euo pipefail

readonly IMAGE="${QIFTOP_PROBE_IMAGE:-alpine:latest}"
readonly NAME="qiftop-systemd-probe-$$"
readonly TARGET_HOST="${QIFTOP_PROBE_TARGET_HOST:-github.com}"
readonly TARGET_PORT="${QIFTOP_PROBE_TARGET_PORT:-22}"
readonly BUILD_DIR="${QIFTOP_BUILD_DIR:-}"
readonly SVC="qiftop-agent"
readonly BUS_NAME="org.qiftop.NetworkAgent1"
readonly CONN_PATH="/org/qiftop/NetworkAgent1/Connections"
readonly CONN_IFACE="org.qiftop.NetworkAgent1.Connections"
readonly IFACE_PATH="/org/qiftop/NetworkAgent1/Interfaces"
readonly IFACE_IFACE="org.qiftop.NetworkAgent1.Interfaces"

skip() { echo "harness: $1; SKIPPING" >&2; exit 77; }
die()  { echo "harness: $1" >&2; exit 70; }

# Install the agent either from a freshly-built .rpm (Fedora SELinux VM
# path) or via `cmake --install` (default Ubuntu path). The rpm path
# additionally exercises the package scriptlets + correct SELinux file
# labels (restorecon on install), which `cmake --install` does not give.
# Set QIFTOP_AGENT_RPM_DIR to a dir containing qiftop-agent-*.rpm to opt in.
install_agent() {
    if [[ -n "${QIFTOP_AGENT_RPM_DIR:-}" ]]; then
        local rpm
        rpm="$(ls "${QIFTOP_AGENT_RPM_DIR}"/qiftop-agent-*.rpm 2>/dev/null | head -1 || true)"
        [[ -n "$rpm" ]] \
            || die "QIFTOP_AGENT_RPM_DIR=${QIFTOP_AGENT_RPM_DIR} but no qiftop-agent-*.rpm there"
        echo "harness: installing qiftop-agent via rpm: $rpm"
        # remove-then-install so a rebuilt SAME-version rpm is actually
        # reinstalled (dnf install of an already-present version is a
        # no-op) AND both scriptlets (postun on remove, post on install)
        # get exercised every run.
        sudo dnf -y remove qiftop-agent >/dev/null 2>&1 || true
        sudo dnf -y --setopt=install_weak_deps=False install "$rpm" >/dev/null \
            || die "dnf install of $rpm failed"
    else
        echo "harness: installing qiftop-agent component from ${BUILD_DIR}"
        sudo cmake --install "$BUILD_DIR" --component qiftop-agent --prefix /usr >/dev/null \
            || die "cmake --install of qiftop-agent failed (is the target built?)"
    fi
}

# SELinux AVC audit. Gated by QIFTOP_SELINUX_AUDIT=1 (off => no-op, so the
# Ubuntu/AppArmor path is unaffected). Greps the audit log for AVC denials
# mentioning qiftop since AUDIT_SINCE (captured just before the agent
# starts). Returns non-zero (and prints the denials) if any are found —
# under the default unconfined_service_t domain there should be NONE, so a
# hit means the agent tripped SELinux and likely needs a policy tweak.
selinux_report() {
    [[ "${QIFTOP_SELINUX_AUDIT:-0}" == "0" ]] && return 0
    local mode denials
    mode="$(getenforce 2>/dev/null || echo Unknown)"
    echo "harness: SELinux mode: ${mode}"
    if ! command -v ausearch >/dev/null 2>&1; then
        echo "harness: ausearch not present; cannot audit SELinux (auditd installed?)" >&2
        return 0
    fi
    denials="$(sudo ausearch -m avc,user_avc -ts "${AUDIT_SINCE:-recent}" 2>/dev/null \
        | grep -i 'qiftop' || true)"
    if [[ -n "$denials" ]]; then
        echo "harness: ==== SELinux AVC denials involving qiftop ====" >&2
        echo "$denials" >&2
        echo "harness: =============================================" >&2
        echo "harness: (inspect with: sudo ausearch -m avc -ts recent | audit2allow -R)" >&2
        return 1
    fi
    echo "harness: SELinux audit clean — no qiftop AVC denials since ${AUDIT_SINCE:-recent}"
    return 0
}

# --- env-supplied value guards ---------------------------------------------
# These values are interpolated into `docker run` argv and an in-container
# `sh -c` string. Minimal sanity guards: reject an image ref that could be
# parsed as a CLI option, and reject host/port values containing shell
# metacharacters.
[[ "$IMAGE" == -* ]] && die "QIFTOP_PROBE_IMAGE must not start with '-' (got '$IMAGE')"
[[ "$TARGET_HOST" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] \
    || die "QIFTOP_PROBE_TARGET_HOST is not a plausible hostname/IP (got '$TARGET_HOST')"
[[ "$TARGET_PORT" =~ ^[0-9]{1,5}$ ]] \
    || die "QIFTOP_PROBE_TARGET_PORT must be numeric (got '$TARGET_PORT')"

# --- preconditions --------------------------------------------------------
command -v systemctl >/dev/null 2>&1 || skip "no systemd (systemctl)"
command -v busctl    >/dev/null 2>&1 || skip "no busctl"
command -v docker    >/dev/null 2>&1 || skip "docker not in PATH"
command -v python3   >/dev/null 2>&1 || skip "python3 not in PATH"
docker info >/dev/null 2>&1           || skip "docker daemon not reachable"
if ! sudo -n true 2>/dev/null; then
    skip "passwordless sudo unavailable (this runner is VM/CI-only)"
fi
[[ -n "$BUILD_DIR" && -d "$BUILD_DIR" ]] || skip "QIFTOP_BUILD_DIR unset/missing"
# Outbound reachability to the flow target: if the host itself can't reach
# it, the container won't either — skip rather than fail spuriously.
if ! timeout 5 bash -c "exec 3<>/dev/tcp/${TARGET_HOST}/${TARGET_PORT}" 2>/dev/null; then
    skip "no outbound TCP to ${TARGET_HOST}:${TARGET_PORT} (offline env?)"
fi

# --- install + start the agent under systemd ------------------------------
# Install the qiftop-agent component (prefix /usr so the binary lands at
# /usr/bin/qiftop-agent, matching the unit's hard-coded ExecStart) — or,
# on the Fedora SELinux path, from the freshly-built .rpm. See install_agent.
install_agent

# Capture the audit cursor JUST before the agent starts so selinux_report
# only considers denials produced by THIS run.
AUDIT_SINCE="$(date '+%T' 2>/dev/null || echo recent)"

sudo systemctl daemon-reload
sudo systemctl restart "$SVC" || die "systemctl restart $SVC failed"

# Wait for the bus name to answer a real method call.
ready=0
for _ in $(seq 1 30); do
    if sudo busctl --system call "$BUS_NAME" "$CONN_PATH" "$CONN_IFACE" \
            GetConnections >/dev/null 2>&1; then
        ready=1; break
    fi
    sleep 0.3
done
[[ $ready -eq 1 ]] || die "agent did not answer GetConnections after restart"

# Sanity: without netns-scan the agent cannot attribute container flows, so
# a failure below would be misleading. Surface it as a skip.
if ! sudo busctl --system get-property "$BUS_NAME" "$IFACE_PATH" \
        "$IFACE_IFACE" Capabilities 2>/dev/null | grep -q "netns-scan"; then
    skip "agent does not advertise netns-scan (no CAP_SYS_ADMIN?) — cannot test container attribution"
fi
echo "harness: agent up and advertising netns-scan"

# Keep the agent warm at a fast cadence so snapshots are fresh.
warm() {
    sudo busctl --system call "$BUS_NAME" "$CONN_PATH" "$CONN_IFACE" \
        SetDesiredIntervalMs u 500 >/dev/null 2>&1 || true
}
warm

# Resolve the target to a single IP on the HOST and dial that IP directly
# from the container. This avoids in-container DNS (busybox resolv flakiness
# caused spurious NOT-FOUND skips) and pins the remote address so stale
# conntrack husks to a different github round-robin IP can't be mistaken for
# this container's flow.
TARGET_IP="$(getent ahostsv4 "$TARGET_HOST" 2>/dev/null | awk '{print $1; exit}')"
[[ -z "$TARGET_IP" ]] && TARGET_IP="$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$TARGET_HOST" 2>/dev/null || true)"
[[ -n "$TARGET_IP" ]] || skip "could not resolve ${TARGET_HOST} to an IPv4 address"

# --- bring up a container holding ONE long-lived external flow ------------
cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; rm -f "${JSON_FILE:-}"; }
trap cleanup EXIT

# `sleep <big> | nc <ip> 22` keeps nc's stdin open (never EOFs) so the
# single nc process holds the SSH pre-auth connection alive. Stable PID +
# stable, conntracked socket = attributable.
docker run -d --rm --name "$NAME" "$IMAGE" \
    sh -c "sleep 3600 | nc ${TARGET_IP} ${TARGET_PORT}" >/dev/null

CID=""
for _ in $(seq 1 20); do
    CID="$(docker inspect -f '{{.Id}}' "$NAME" 2>/dev/null || true)"
    [[ -n "$CID" ]] && break
    sleep 0.2
done
[[ -n "$CID" ]] || die "container did not start within 4s"
CID_SHORT="${CID:0:12}"

# Read the container's IPv4. The top-level .NetworkSettings.IPAddress is
# populated by older docker but is EMPTY/absent on newer docker (and
# podman-docker) where the address only lives under
# .NetworkSettings.Networks.<net>.IPAddress. Walk the Networks map first
# and fall back to the legacy top-level field for portability.
cont_ip_of() {
    local ip
    ip="$(docker inspect -f \
        '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' \
        "$NAME" 2>/dev/null || true)"
    [[ -z "$ip" ]] && ip="$(docker inspect -f \
        '{{.NetworkSettings.IPAddress}}' "$NAME" 2>/dev/null || true)"
    printf '%s' "$ip"
}
CONT_IP=""
for _ in $(seq 1 20); do
    CONT_IP="$(cont_ip_of)"
    [[ -n "$CONT_IP" ]] && break
    sleep 0.2
done
[[ -n "$CONT_IP" ]] || die "container has no IPv4 address"
echo "harness: container ${CID_SHORT} ip=${CONT_IP} dialing ${TARGET_IP}:${TARGET_PORT} (${TARGET_HOST})"

# Wait until the container's outbound connection is actually ESTABLISHED
# before we start asserting — otherwise we race the TCP handshake and may
# time out before conntrack ever sees the flow.
established=0
for _ in $(seq 1 30); do
    if docker exec "$NAME" sh -c 'netstat -tn 2>/dev/null || ss -tn 2>/dev/null' \
            2>/dev/null | grep -q "ESTABLISHED\|ESTAB"; then
        established=1; break
    fi
    sleep 0.3
done
[[ $established -eq 1 ]] \
    || skip "container could not establish a TCP flow to ${TARGET_IP}:${TARGET_PORT} (network egress blocked?)"
echo "harness: container flow established; letting the agent's conntrack view settle"
# GetConnections returns the agent's last EMITTED snapshot (cached m_last,
# refreshed once per poll tick), so give the monitor a few ticks to pick up
# the just-created flow before we start asserting.
sleep 5

# --- query the agent over DBus + assert attribution -----------------------
# Match the flow by (container local IP, target remote IP, remote port). The
# agent reports the pre-NAT original tuple, so localAddress == container IP.
# Aggregate across all matching rows: an attributed row wins (stale conntrack
# husks are unattributed and must not short-circuit the result).
parse_and_check() {
    # $1 = file with busctl --json=short output.
    # NOTE: the program is fed to `python3 -` on stdin via the heredoc, so
    # the JSON MUST be passed as a file path argument, not piped on stdin.
    python3 - "$1" "$CONT_IP" "$TARGET_IP" "$TARGET_PORT" "$CID_SHORT" <<'PY'
import json, sys
jsonfile, cont_ip, tgt_ip, rport, cid = (
    sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5])
with open(jsonfile) as fh:
    doc = json.load(fh)
conns = doc["data"][0]   # the a(...) array of connection tuples
present = False
for c in conns:
    # [2]=localAddress [5]=remoteAddress [6]=remotePort
    # [18]=containerRuntime [19]=containerId
    if c[2] == cont_ip and c[5] == tgt_ip and c[6] == rport:
        present = True
        runtime, cid_field = c[18], c[19]
        if runtime == "docker" and cid_field and cid_field[:12] == cid[:12]:
            print(f"MATCH runtime={runtime} id={cid_field}")
            sys.exit(0)
if present:
    # Flow present but not (yet) attributed — keep polling; only a terminal
    # FOUND-UNATTRIBUTED indicates the systemd-sandbox regression.
    print("FOUND-UNATTRIBUTED")
    sys.exit(2)
print("NOT-FOUND")
sys.exit(3)
PY
}

JSON_FILE="$(mktemp)"
last="NOT-FOUND"
for _ in $(seq 1 40); do
    # Warm FIRST, then wait a tick, THEN read — GetConnections returns the
    # cached m_last, so we need the cadence hint to have driven a fresh poll
    # before we query (mirrors the proven manual sequence).
    warm
    sleep 0.8
    if sudo busctl --system --json=short call "$BUS_NAME" "$CONN_PATH" \
            "$CONN_IFACE" GetConnections >"$JSON_FILE" 2>/dev/null \
            && [[ -s "$JSON_FILE" ]]; then
        if out="$(parse_and_check "$JSON_FILE")"; then
            echo "harness: $out"
            echo "PASS: systemd agent attributed the container flow over DBus"
            # Attribution worked; now make sure SELinux didn't merely *warn*
            # while letting it through (gated; no-op off the Fedora path).
            if ! selinux_report; then
                echo "FAIL: attribution succeeded but SELinux logged qiftop AVC" \
                     "denials (see above) — the agent needs an SELinux policy" \
                     "adjustment before it can run confined." >&2
                exit 1
            fi
            exit 0
        fi
        last="$out"
    else
        last="BUSCTL-EMPTY"
    fi
done
echo "harness: giving up after polling; last=${last}" >&2
echo "harness: diagnostic dump of agent rows with remotePort=${TARGET_PORT}:" >&2
python3 - "$JSON_FILE" "$TARGET_PORT" >&2 <<'PY' || true
import json, sys
try:
    doc = json.load(open(sys.argv[1]))
except Exception as e:
    print("  (could not parse last snapshot:", e, ")"); sys.exit()
rp = int(sys.argv[2])
rows = [(c[2], c[5], c[6], c[18], c[19]) for c in doc["data"][0] if c[6] == rp]
for r in rows[:12]:
    print("   ", r)
print("  total flows:", len(doc["data"][0]), " rows@port:", len(rows))
PY
echo "harness: container netstat:" >&2
docker exec "$NAME" sh -c 'netstat -tn 2>/dev/null || ss -tn 2>/dev/null' 2>/dev/null | grep -i estab >&2 || echo "  (none)" >&2
echo "harness: host conntrack for ${CONT_IP}:" >&2
sudo conntrack -L 2>/dev/null | grep "$CONT_IP" >&2 || echo "  (none in host conntrack)" >&2

# Distinguish the two failure shapes:
#   FOUND-UNATTRIBUTED -> the flow is visible but the agent couldn't attribute
#                         it. This is the systemd-sandbox regression we guard —
#                         BUT only if the socket is still live (an expired SSH
#                         grace leaves an unattributable conntrack husk).
#   NOT-FOUND          -> the flow never reached conntrack at all (env issue:
#                         no masquerade, etc). Skip rather than cry wolf — the
#                         in-process probe runners cover the resolver logic.
if [[ "$last" == FOUND-UNATTRIBUTED* ]]; then
    if docker exec "$NAME" sh -c 'netstat -tn 2>/dev/null || ss -tn 2>/dev/null' \
            2>/dev/null | grep -qi estab; then
        echo "FAIL: container flow has a LIVE socket and is visible to the agent" >&2
        echo "      but UNATTRIBUTED (${last}). The systemd sandbox almost" >&2
        echo "      certainly blocked NetnsScanner — check RestrictNamespaces /" >&2
        echo "      CAP_SYS_ADMIN in the unit (AGENTS.md §8a)." >&2
        # On the Fedora path this is the smoking gun: SELinux (not the
        # seccomp RestrictNamespaces filter) may be denying setns/sock_diag.
        # Dump any qiftop AVC denials to pinpoint which.
        selinux_report || true
        exit 1
    fi
    skip "flow visible but its socket already closed (SSH grace expired?) — \
unattributable husk, not a regression"
fi
skip "container flow never appeared in the agent snapshot (no conntracked flow); env issue, not an attribution regression"
