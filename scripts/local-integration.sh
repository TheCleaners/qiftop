#!/usr/bin/env bash
# Run the attribution Tier-2 integration tests inside a disposable
# Vagrant VM under tests/integration/vagrant/. Isolates the run from the
# host's logind / sudo-timeout / kernel-module state, matching what CI
# sees. Two VMs are available via --distro:
#
#   ubuntu (default) — full runner set (docker/podman/k3d/k8s/systemd-dbus)
#   fedora           — SELinux-ENFORCING; systemd-dbus runner installs the
#                      real .rpm and audits for qiftop SELinux AVC denials
#
#   ./scripts/local-integration.sh                    # ubuntu, docker + podman
#   ./scripts/local-integration.sh --runtime podman   # ubuntu, just podman
#   ./scripts/local-integration.sh --runtime docker -v
#   ./scripts/local-integration.sh --distro fedora    # fedora, systemd-dbus + SELinux
#   ./scripts/local-integration.sh --halt             # power down when done
#   ./scripts/local-integration.sh --destroy          # nuke the VM(s)
#
# First run does a `vagrant up` (a few minutes for box download +
# provisioning). Subsequent runs are an rsync + ctest, ~30s.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VAGRANT_DIR="$SCRIPT_DIR/../tests/integration/vagrant"

RUNTIME=""        # empty == default for the chosen distro
DISTRO="ubuntu"   # which VM: ubuntu (full) or fedora (SELinux/rpm)
VERBOSE=0
HALT=0
DESTROY=0

usage() {
    sed -n '2,21p' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runtime) RUNTIME="$2"; shift 2 ;;
        --runtime=*) RUNTIME="${1#*=}"; shift ;;
        --distro) DISTRO="$2"; shift 2 ;;
        --distro=*) DISTRO="${1#*=}"; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --halt) HALT=1; shift ;;
        --destroy) DESTROY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage 2 ;;
    esac
done

case "$DISTRO" in
    ubuntu|fedora) ;;
    *) echo "unknown distro: $DISTRO (want ubuntu|fedora)" >&2; exit 2 ;;
esac
# The Vagrant machine name matches the distro.
MACHINE="$DISTRO"
# The Fedora VM exists to exercise the SELinux + rpm path, so default it
# to the systemd-dbus runner (it has no k3d/k8s provisioned anyway).
if [[ "$DISTRO" == "fedora" && -z "$RUNTIME" ]]; then
    RUNTIME="systemd-dbus"
fi

# Default to the system libvirt instance — see Vagrantfile for the
# rationale (rootless session mode needs invasive qemu-bridge-helper
# setup). Polkit allows the 'libvirt' group passwordless access.
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"

# Surface the libvirt-group requirement early with a helpful message
# instead of letting it fail later as a polkit prompt mid-run.
if [[ "$LIBVIRT_DEFAULT_URI" == "qemu:///system" ]] && ! id -nG | tr ' ' '\n' | grep -qx libvirt; then
    cat >&2 <<EOF
error: $USER is not in the 'libvirt' group, so qemu:///system access
will trigger a polkit/pkexec password prompt. One-time fix:

    sudo usermod -aG libvirt \$USER
    newgrp libvirt          # or log out + back in for it to stick

Then re-run this script. (To use rootless libvirt instead, export
LIBVIRT_DEFAULT_URI=qemu:///session — but you'll also need to fix
/etc/qemu/bridge.conf + setuid the bridge helper. Group is cleaner.)
EOF
    exit 1
fi

cd "$VAGRANT_DIR"

if [[ $DESTROY -eq 1 ]]; then
    exec vagrant destroy -f
fi

# Bring the chosen VM up if needed (idempotent; fast no-op when running).
if ! vagrant status "$MACHINE" --machine-readable | grep -q ',state,running$'; then
    echo "==> bringing '$MACHINE' VM up (first run downloads the box)"
    vagrant up "$MACHINE" --provider=libvirt
fi

echo "==> syncing repo into '$MACHINE' VM"
vagrant rsync "$MACHINE"

# Build the test selector. ctest -R matches by regex so 'attribution_'
# alone catches both attribution_docker and attribution_podman.
if [[ -z $RUNTIME ]]; then
    CTEST_FILTER='attribution_'
else
    case "$RUNTIME" in
        docker|podman|k3d|k8s) CTEST_FILTER="attribution_${RUNTIME}\$" ;;
        systemd-dbus|systemd_dbus) CTEST_FILTER='attribution_systemd_dbus$' ;;
        *) echo "unknown runtime: $RUNTIME" >&2; exit 2 ;;
    esac
fi

CTEST_VERBOSE=""
[[ $VERBOSE -eq 1 ]] && CTEST_VERBOSE="-V"

# Fedora path: additionally build the .rpm so the systemd-dbus runner can
# install the real package, and turn on the SELinux AVC audit. These are
# exported into the guest shell and forwarded through sudo via
# --preserve-env so the runner (invoked under sudo by ctest) sees them.
EXTRA_BUILD=":"            # no-op on ubuntu
RUNNER_ENV_EXPORTS=":"     # no-op on ubuntu
PRESERVE_ENV="PATH"
if [[ "$DISTRO" == "fedora" ]]; then
    EXTRA_BUILD='( cd build && cpack -G RPM )'
    RUNNER_ENV_EXPORTS='export QIFTOP_AGENT_RPM_DIR=/home/vagrant/qiftop/build QIFTOP_SELINUX_AUDIT=1'
    PRESERVE_ENV="PATH,QIFTOP_AGENT_RPM_DIR,QIFTOP_SELINUX_AUDIT"
fi

echo "==> running ctest filter '$CTEST_FILTER' inside '$MACHINE' VM"
set +e
vagrant ssh "$MACHINE" -c "set -euo pipefail
    cd /home/vagrant/qiftop
    cmake -S . -B build -G Ninja \\
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
        -DQIFTOP_BUILD_TESTS=ON \\
        -DQIFTOP_BUILD_INTEGRATION_TESTS=OFF \\
        -DQIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON
    # The qiftop-agent target has a POST_BUILD cpack hook that regenerates
    # BOTH packages, which needs the qiftop GUI binary on disk. Under Ninja's
    # parallel scheduling the hook can fire before the GUI target finishes,
    # so build the GUI to completion FIRST, then the agent + probe.
    cmake --build build --target qiftop -j
    cmake --build build --target qiftop-attribution-probe qiftop-agent -j
    $EXTRA_BUILD
    $RUNNER_ENV_EXPORTS
    sudo --preserve-env=$PRESERVE_ENV ctest --test-dir build \\
        -R '$CTEST_FILTER' --output-on-failure $CTEST_VERBOSE
"
RC=$?
set -e

if [[ $HALT -eq 1 ]]; then
    echo "==> halting '$MACHINE' VM"
    vagrant halt "$MACHINE"
fi

exit $RC
