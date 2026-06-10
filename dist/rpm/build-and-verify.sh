#!/usr/bin/env bash
# Build the qiftop RPMs inside this Fedora container, then install + verify
# them. Intended to run as root inside fedora:44 with the repo at /src (ro).
set -euo pipefail

echo "===== install build deps ====="
dnf -y --setopt=install_weak_deps=False --setopt=fastestmirror=True \
    --setopt=retries=10 --setopt=timeout=30 install \
    cmake ninja-build gcc-c++ pkgconf-pkg-config rpm-build \
    qt6-qtbase-devel qt6-qtbase-private-devel mesa-libGL-devel \
    libnl3-devel libnfnetlink-devel libnetfilter_conntrack-devel \
    dbus-daemon systemd shadow-utils >/tmp/dnf.log 2>&1 \
    || { tail -25 /tmp/dnf.log; exit 1; }
echo "deps OK"

echo "===== copy source + configure ====="
mkdir -p /work && cp -a /src/. /work/ && cd /work
rm -rf build build-* CMakeCache.txt
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQIFTOP_BUILD_TESTS=OFF \
    -DQIFTOP_AUTO_PACKAGE=OFF >/tmp/cfg.log 2>&1 || { tail -30 /tmp/cfg.log; exit 1; }
echo "configure OK"

echo "===== build ====="
cmake --build build --target qiftop qiftop-agent -j"$(nproc)" >/tmp/build.log 2>&1 \
    || { tail -30 /tmp/build.log; exit 1; }
echo "build OK"

echo "===== cpack -G RPM ====="
( cd build && cpack -G RPM -V >/tmp/cpack.log 2>&1 ) || { tail -40 /tmp/cpack.log; exit 1; }
ls -1 build/*.rpm
echo

echo "===== rpm metadata (name-version-release.dist.arch) ====="
for f in build/*.rpm; do
    echo "--- $f ---"
    rpm -qp --qf 'Name: %{NAME}\nVersion: %{VERSION}\nRelease: %{RELEASE}\nArch: %{ARCH}\nLicense: %{LICENSE}\n' "$f"
    echo "Requires:"; rpm -qpR "$f" | sed 's/^/    /'
    echo "Recommends:"; rpm -qp --recommends "$f" 2>/dev/null | sed 's/^/    /' || true
done
echo

echo "===== install via dnf (resolves auto-req library deps) ====="
dnf -y --setopt=install_weak_deps=False install ./build/qiftop-agent-*.rpm ./build/qiftop-*.rpm \
    >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }
echo "install OK"
echo

echo "===== VERIFY ====="
echo "qiftop --version:        $(QT_QPA_PLATFORM=offscreen qiftop --version 2>/dev/null || echo FAIL)"
echo "qiftop-agent --version:  $(qiftop-agent --version 2>/dev/null || echo FAIL)"
echo "netdev group:            $(getent group netdev || echo MISSING)"
echo -n "systemd unit:            "; ls /usr/lib/systemd/system/qiftop-agent.service /lib/systemd/system/qiftop-agent.service 2>/dev/null | head -1 || echo MISSING
echo -n "dbus policy:             "; ls /usr/share/dbus-1/system.d/org.qiftop.NetworkAgent1.conf 2>/dev/null || echo MISSING
echo -n "dbus activation:         "; ls /usr/share/dbus-1/system-services/org.qiftop.NetworkAgent1.service 2>/dev/null || echo MISSING
echo -n "nft shim:                "; ls /usr/share/qiftop/qiftop-conntrack.nft 2>/dev/null || echo MISSING
echo -n "desktop file:            "; ls /usr/share/applications/qiftop.desktop 2>/dev/null || echo MISSING
echo -n "icon:                    "; ls /usr/share/icons/hicolor/scalable/apps/qiftop.svg 2>/dev/null || echo MISSING
echo "config (should be noreplace):"; rpm -qc qiftop-agent | sed 's/^/    /'
echo -n "config %config flag:     "; rpm -q --qf '[%{FILENAMES} %{FILEFLAGS:fflags}\n]' qiftop-agent | grep agent.conf || echo "not flagged"
echo
echo "===== which packages own what (sanity) ====="
rpm -q qiftop qiftop-agent
echo "ALL CHECKS DONE"
