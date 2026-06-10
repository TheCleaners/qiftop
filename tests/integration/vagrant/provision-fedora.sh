#!/usr/bin/env bash
# Provisioning for the qiftop Fedora integration VM. SELinux-ENFORCING
# host whose reason to exist is exercising the agent's systemd unit +
# DBus policy under SELinux, and the real .rpm install path — the two
# things the Ubuntu VM (provision.sh) structurally cannot cover.
#
# Idempotent: `vagrant provision` is a no-op on a second run except for
# dnf metadata refreshes.
#
# Deliberately NARROWER than provision.sh: no k3d / k0s / kubectl. The
# Fedora VM is scoped to the systemd-dbus runner (docker is the flow
# generator). Tier-1 cgroup fixtures + the Ubuntu VM already pin the
# k8s chain shapes; duplicating them here would just slow bring-up.
set -euo pipefail

echo "==> dnf install build + runtime deps"
# Library deps mirror dist/rpm/build-and-verify.sh + what the agent rpm
# pulls in. rpm-build is needed for `cpack -G RPM`. audit + setools give
# us ausearch / audit2allow for the SELinux AVC inspection that is the
# whole point of this VM. moby-engine provides docker on Fedora.
dnf -y --setopt=install_weak_deps=False install \
    gcc-c++ cmake ninja-build pkgconf-pkg-config ca-certificates curl \
    qt6-qtbase-devel qt6-qtbase-private-devel mesa-libGL-devel \
    libnl3-devel libnfnetlink-devel libnetfilter_conntrack-devel \
    iproute iputils nmap-ncat conntrack-tools \
    rpm-build \
    moby-engine \
    podman netavark \
    rsync python3 \
    audit policycoreutils policycoreutils-python-utils setroubleshoot-server

echo "==> enable + start docker (moby-engine)"
systemctl enable --now docker || true
usermod -aG docker vagrant || true

echo "==> enable + start auditd (SELinux AVC capture for ausearch)"
# On most Fedora cloud/generic images auditd is already present+running;
# enable --now is a no-op then. The systemd-dbus runner greps ausearch
# for qiftop AVC denials, so auditd MUST be collecting.
systemctl enable --now auditd || true

echo "==> ensure SELinux is ENFORCING (the entire purpose of this VM)"
# Fedora ships enforcing by default, so setenforce 1 is usually a no-op.
# Persist it in case a custom box shipped permissive. (disabled->enforcing
# would need a relabel + reboot; Fedora default is enforcing so we never
# hit that.)
setenforce 1 2>/dev/null || true
if [[ -f /etc/selinux/config ]]; then
    sed -i 's/^SELINUX=.*/SELINUX=enforcing/' /etc/selinux/config || true
fi
echo "    SELinux mode now: $(getenforce 2>/dev/null || echo unknown)"

echo "==> NOPASSWD sudo for the vagrant user"
cat >/etc/sudoers.d/90-vagrant-nopasswd <<'EOF'
vagrant ALL=(ALL) NOPASSWD:ALL
Defaults:vagrant !requiretty
Defaults:vagrant timestamp_timeout=-1
EOF
chmod 0440 /etc/sudoers.d/90-vagrant-nopasswd

echo "==> pre-pull alpine so the first test run isn't dominated by pull time"
docker pull alpine:latest || true

echo "==> conntrack module for the ConntrackMonitor / agent"
modprobe nf_conntrack || true
echo nf_conntrack >/etc/modules-load.d/qiftop.conf

echo "==> done."
