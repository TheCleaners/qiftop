#!/usr/bin/env bash
# Provisioning for the qiftop integration-test VM. Idempotent —
# `vagrant provision` should be a no-op on the second run except for
# apt index refreshes.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "==> apt update + install"
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config ca-certificates \
    qt6-base-dev libqt6dbus6 \
    libnl-3-dev libnl-route-3-dev libnetfilter-conntrack-dev \
    iproute2 iputils-ping netcat-openbsd \
    docker.io \
    podman netavark \
    clang libbpf-dev linux-tools-common linux-tools-generic \
    rsync

echo "==> ensure vagrant user can run docker without sudo"
usermod -aG docker vagrant || true

echo "==> install kubectl + k3d (for k8s attribution runner)"
# kubectl: pinned to a recent stable. Apt has it as 'kubectl' in
# ubuntu's universe but the version is ancient — pull from the
# official binary release.
KUBECTL_VERSION="v1.30.4"
if ! command -v kubectl >/dev/null 2>&1 || \
   [[ "$(kubectl version --client -o json 2>/dev/null | grep -o '"gitVersion":"[^"]*"' | head -1)" != *"$KUBECTL_VERSION"* ]]; then
    curl -fsSL -o /usr/local/bin/kubectl \
        "https://dl.k8s.io/release/${KUBECTL_VERSION}/bin/linux/amd64/kubectl"
    chmod +x /usr/local/bin/kubectl
fi

# k3d: spins up k3s clusters as docker containers. Tiny (~50MB), fast
# (~20s cold), and shipped as a single static binary.
K3D_VERSION="v5.7.4"
if ! command -v k3d >/dev/null 2>&1 || \
   [[ "$(k3d version 2>/dev/null | head -1)" != *"$K3D_VERSION"* ]]; then
    curl -fsSL "https://github.com/k3d-io/k3d/releases/download/${K3D_VERSION}/k3d-linux-amd64" \
        -o /usr/local/bin/k3d
    chmod +x /usr/local/bin/k3d
fi

echo "==> install k0s (single-binary 'naked' k8s — runs directly on the VM,"
echo "    NOT inside a docker container like k3d. Exercises the cgroup path"
echo "    shape we'd see on a real production k8s node.)"
K0S_VERSION="v1.30.4+k0s.0"
if ! command -v k0s >/dev/null 2>&1 || \
   [[ "$(k0s version 2>/dev/null)" != "$K0S_VERSION" ]]; then
    # k0s ships a curl|sh installer; we run their pinned binary directly
    # for reproducibility.
    curl -fsSL "https://github.com/k0sproject/k0s/releases/download/${K0S_VERSION}/k0s-${K0S_VERSION}-amd64" \
        -o /usr/local/bin/k0s
    chmod +x /usr/local/bin/k0s
fi

# Install k0s as a systemd service if it isn't already. --single =
# all-in-one controller+worker on one node, no etcd cluster, perfect
# for an integration runner. Idempotent: 'k0s install' refuses to
# overwrite an existing unit, so we suppress that one specific error.
if [[ ! -f /etc/systemd/system/k0scontroller.service ]]; then
    k0s install controller --single
fi
# Start it lazily — the test runner is what cares whether it's up.
# Starting at provision time would extend `vagrant up` by another
# ~20s for every developer, even those running only docker tests.

echo "==> NOPASSWD sudo for the vagrant user (override host sudoers timeout)"
# Vagrant boxes already ship this, but be explicit so a re-provision on
# a customized box can't silently regress and reintroduce password prompts
# mid-test.
cat >/etc/sudoers.d/90-vagrant-nopasswd <<'EOF'
vagrant ALL=(ALL) NOPASSWD:ALL
Defaults:vagrant !requiretty
Defaults:vagrant timestamp_timeout=-1
EOF
chmod 0440 /etc/sudoers.d/90-vagrant-nopasswd

echo "==> pre-pull alpine image so the first test run isn't dominated by pull time"
docker pull alpine:3.20 || true
podman pull docker.io/library/alpine:3.20 || true

echo "==> pre-pull k3s image so the first k3d cluster create isn't dominated by it"
docker pull rancher/k3s:v1.30.4-k3s1 || true

echo "==> conntrack module: ensure it's loaded for the ConntrackMonitor tests"
modprobe nf_conntrack || true
echo nf_conntrack >/etc/modules-load.d/qiftop.conf

echo "==> done."
