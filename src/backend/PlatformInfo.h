#pragma once

#include <QHostAddress>
#include <QSet>

#include <utility>

// Tiny platform-inspection façade. Centralises every "ask the kernel
// what the current host looks like" probe so the agent and the UI
// don't sprinkle `#ifdef Q_OS_*` or procfs reads across high-level code.
//
// EVERY function here:
//   * Must compile on every supported platform.
//   * Returns a sensible, well-documented fallback when the underlying
//     probe isn't available on the running OS (rather than failing).
//   * Is cheap to call repeatedly (cached or fast syscalls); callers
//     refresh every tick because addresses / port range can change at
//     runtime (DHCP, VPN up/down, sysctl edits).
//
// Why this exists rather than each call-site calling getifaddrs() /
// reading /proc directly:
//   1. Portability: macOS/FreeBSD have no /proc, Windows has neither
//      /proc nor getifaddrs. New ports add one impl, not 5 grep hits.
//   2. Testability: a future header-only FakePlatformInfo can replace
//      this without dragging in the kernel.
//   3. Layering: callers in `ui/` and `agent/` must remain free of
//      direct platform headers (AGENTS.md §2 layering rules).
namespace qiftop::platform {

// All non-loopback IP addresses currently assigned to local interfaces.
// Used by direction-inference and "is this peer actually us?" checks.
// On platforms without a host-address probe, returns an empty set —
// callers MUST treat that as "couldn't tell" and fall back to other
// signals (ephemeral port comparison, conntrack ORIGINAL/REPLY tuple).
[[nodiscard]] QSet<QHostAddress> localAddresses();

// All loopback addresses currently assigned (127.0.0.0/8 + ::1).
[[nodiscard]] QSet<QHostAddress> loopbackAddresses();

// Kernel-configured ephemeral source-port range for outgoing
// connections, as (low, high). On Linux this is sysctl
// `net.ipv4.ip_local_port_range` (the IPv6 stack mirrors it). On
// platforms without a configurable range, returns IANA's recommended
// (49152, 65535) per RFC 6056.
//
// Guaranteed: low < high, low >= 1024, high <= 65535.
[[nodiscard]] std::pair<quint16, quint16> ephemeralPortRange();

// Resolve a numeric uid to a login name (e.g. 1000 → "ines").
// Returns an empty string when the uid has no passwd entry or the
// platform has no user database (the caller then shows the numeric
// uid). Cheap + cached internally; safe to call per row render.
[[nodiscard]] QString userNameForUid(uint uid);

// True if the user identified by `uid` is a member of `groupName` — either
// as their primary group or a supplementary one. Returns false when the
// user or group can't be resolved, or on platforms with no group database.
// Used by the agent to gate cross-UID process-detail disclosure on an
// admin-configured group allowlist (e.g. "wheel"). Cheap + cached.
[[nodiscard]] bool userInGroup(uint uid, const QString &groupName);

// True when this process runs with elevated privileges (effective uid 0)
// but the per-user config directory QSettings would write to belongs to a
// DIFFERENT, unprivileged user — i.e. persisting settings now would create
// root-owned files inside someone else's $HOME.
//
// This happens whenever qiftop/nqiftop is run privileged with a foreign
// $HOME: `sudo -E nqiftop`, a pkexec/su variant that preserves HOME, or the
// GUI self-elevation re-exec (which forwards HOME so Qt finds the user's
// theme/font config — see util/PrivilegeEscalator). Callers MUST treat a
// true result as "read settings, but do NOT write them back" so the
// unprivileged owner's config tree is never polluted with root-owned files.
//
// Returns false for any unprivileged process (the common case) and on
// platforms without a POSIX uid/ownership model.
[[nodiscard]] bool settingsWriteWouldEscalate();

} // namespace qiftop::platform
