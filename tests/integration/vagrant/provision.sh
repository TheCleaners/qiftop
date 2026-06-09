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
    rsync

echo "==> ensure vagrant user can run docker without sudo"
usermod -aG docker vagrant || true

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

echo "==> conntrack module: ensure it's loaded for the ConntrackMonitor tests"
modprobe nf_conntrack || true
echo nf_conntrack >/etc/modules-load.d/qiftop.conf

echo "==> done."
