#!/usr/bin/env bash
# Run the attribution Tier-2 integration tests inside the disposable
# ubuntu-24.04 Vagrant VM under tests/integration/vagrant/. This
# isolates the run from the host's logind / sudo-timeout / kernel-
# module state, matching what CI sees.
#
#   ./scripts/local-integration.sh                    # docker + podman
#   ./scripts/local-integration.sh --runtime podman   # just podman
#   ./scripts/local-integration.sh --runtime docker -v
#   ./scripts/local-integration.sh --halt             # power down when done
#   ./scripts/local-integration.sh --destroy          # nuke the VM
#
# First run does a `vagrant up` (a few minutes for box download +
# provisioning). Subsequent runs are an rsync + ctest, ~30s.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VAGRANT_DIR="$SCRIPT_DIR/../tests/integration/vagrant"

RUNTIME=""        # empty == both
VERBOSE=0
HALT=0
DESTROY=0

usage() {
    sed -n '2,20p' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --runtime) RUNTIME="$2"; shift 2 ;;
        --runtime=*) RUNTIME="${1#*=}"; shift ;;
        -v|--verbose) VERBOSE=1; shift ;;
        --halt) HALT=1; shift ;;
        --destroy) DESTROY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage 2 ;;
    esac
done

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

# Bring the VM up if needed (idempotent; fast no-op when already running).
if ! vagrant status --machine-readable | grep -q ',state,running$'; then
    echo "==> bringing VM up (first run downloads ~600MB box)"
    vagrant up --provider=libvirt
fi

echo "==> syncing repo into VM"
vagrant rsync

# Build the test selector. ctest -R matches by regex so 'attribution_'
# alone catches both attribution_docker and attribution_podman.
if [[ -z $RUNTIME ]]; then
    CTEST_FILTER='attribution_'
else
    case "$RUNTIME" in
        docker|podman|k3d|k8s) CTEST_FILTER="attribution_${RUNTIME}\$" ;;
        *) echo "unknown runtime: $RUNTIME" >&2; exit 2 ;;
    esac
fi

CTEST_VERBOSE=""
[[ $VERBOSE -eq 1 ]] && CTEST_VERBOSE="-V"

echo "==> running ctest filter '$CTEST_FILTER' inside VM"
set +e
vagrant ssh -c "set -euo pipefail
    cd /home/vagrant/qiftop
    cmake -S . -B build -G Ninja \\
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
        -DQIFTOP_BUILD_TESTS=ON \\
        -DQIFTOP_BUILD_INTEGRATION_TESTS=OFF \\
        -DQIFTOP_BUILD_ATTRIBUTION_INTEGRATION=ON
    cmake --build build --target qiftop-attribution-probe -j
    sudo --preserve-env=PATH ctest --test-dir build \\
        -R '$CTEST_FILTER' --output-on-failure $CTEST_VERBOSE
"
RC=$?
set -e

if [[ $HALT -eq 1 ]]; then
    echo "==> halting VM"
    vagrant halt
fi

exit $RC
