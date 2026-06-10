# RPM %post scriptlet for qiftop-agent.
# RPM semantics: $1 == 1 on first install, $1 == 2 on upgrade.
# No `set -e` — a failing scriptlet command must not abort the dnf
# transaction; every step is guarded with `|| true`.

# Ensure the `netdev` group exists. The DBus policy in
# /usr/share/dbus-1/system.d gates access to org.qiftop.NetworkAgent1 on
# membership of this group; users not in it fall back to the GUI's
# in-process (self-elevated) backend.
if ! getent group netdev >/dev/null 2>&1; then
    groupadd -r netdev || true
fi

# Best-effort: enroll the invoking user into `netdev` so the GUI can reach
# the agent immediately after install. `sudo dnf install` sets $SUDO_USER;
# a PackageKit/pkexec front-end sets $PKEXEC_UID. Skip root / already-members.
target_user=""
if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
    target_user="${SUDO_USER}"
elif [ -n "${PKEXEC_UID:-}" ]; then
    target_user="$(getent passwd "${PKEXEC_UID}" 2>/dev/null | cut -d: -f1 || true)"
fi
if [ -n "${target_user}" ] && getent passwd "${target_user}" >/dev/null 2>&1; then
    if ! id -nG "${target_user}" 2>/dev/null | tr ' ' '\n' | grep -qx netdev; then
        usermod -a -G netdev "${target_user}" || true
        echo "qiftop-agent: added user '${target_user}' to group 'netdev'." >&2
        echo "qiftop-agent: log out and back in (or run 'newgrp netdev') for it to take effect." >&2
    fi
fi

# Pick up the freshly-installed unit + DBus policy.
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

exit 0
