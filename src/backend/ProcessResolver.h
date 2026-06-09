#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "Connection.h"

namespace qiftop::backend {

// Per-process metadata that may be attached to a Connection.
//
// LIFETIME: snapshot value. The resolver caches PID metadata internally but
// hands callers an owned copy so PID reuse between snapshots doesn't tear
// the model.
struct ProcessInfo {
    qint32  pid = 0;
    QString comm;      // short process name (kernel-truncated to 15 bytes)
    QString cmdline;   // full argv joined by spaces
    QString exe;       // resolved /proc/<pid>/exe target (or platform equivalent)
    quint32 uid = 0;

    // A ProcessInfo with pid==0 is the sentinel "not attributed" value.
    // Resolvers MUST set pid > 0 on every meaningful result so consumers
    // can use this flag without inspecting individual fields.
    [[nodiscard]] bool valid() const { return pid > 0; }

    friend bool operator==(const ProcessInfo &, const ProcessInfo &) = default;
};

// Container / cgroup-scope metadata that may be attached to a Connection.
//
// `runtime` is the lowercase runtime name (`"docker"`, `"containerd"`,
// `"podman"`, `"systemd"`, `"lxc"`, ...). `id` is the short, displayable
// identifier — typically the first 12 hex chars of the container ID, or
// the systemd unit name (`"unit:foo.service"`) for non-container scopes.
// `name` is the friendly name (e.g. `"happy_einstein"`); empty when the
// resolver wasn't asked to resolve names or doesn't know how.
struct ContainerInfo {
    QString runtime;
    QString id;
    QString name;

    [[nodiscard]] bool valid() const { return !runtime.isEmpty() && !id.isEmpty(); }

    friend bool operator==(const ContainerInfo &, const ContainerInfo &) = default;
};

// Abstract per-flow attribution resolver.
//
// Implementations live in `backend/<platform>/` (linux: SockDiag + Cgroup +
// Netns) or `backend/null/` (universal no-op fallback). Each resolver
// composes one or more underlying data sources and advertises what it
// actually has at runtime via capabilities().
//
// THREADING
//   resolve*() may be called from the agent's ConntrackMonitor worker
//   thread on every conntrack snapshot. Implementations MUST be
//   reentrant-safe across resolveFlow / resolveContainerForPid calls and
//   should maintain bounded caches with a short TTL (~1s) since the same
//   PIDs and containers are queried repeatedly.
//
// CAPABILITIES
//   Tokens use the same dash-separated lowercase grammar as the
//   InterfacesService::capabilities() list. v0.2 introduces:
//     "process-attribution"   — resolveFlow returns useful ProcessInfo
//     "container-attribution" — resolveContainerForPid returns useful ContainerInfo
//     "netns-scan"            — process attribution sees container netns flows
//   A resolver MUST omit a token when the underlying API isn't reachable
//   at runtime (e.g. kernel doesn't support NETLINK_SOCK_DIAG, /proc is
//   unreadable, etc.) even if the code path was compiled in.
class ProcessResolver {
public:
    virtual ~ProcessResolver() = default;

    // One-shot startup probe. Implementations open sockets, read sysfs
    // probes, etc., and return true if any portion of the resolver
    // became usable. Returning false does NOT abort startup — the
    // caller treats it as "advertise no tokens, pass through to Null
    // semantics" — but is logged.
    virtual bool initialize() = 0;

    // Runtime-detected capability tokens. Subset of the compile-time
    // feature set, intersected with whatever the runtime probe found.
    [[nodiscard]] virtual QStringList capabilities() const = 0;

    // Look up the process owning the local end of `flow`. Returns
    // nullopt when the resolver cannot attribute (flow originates from
    // a netns it doesn't scan, is purely forwarded, or the underlying
    // socket has already closed).
    [[nodiscard]] virtual std::optional<ProcessInfo>
        resolveFlow(const Connection &flow) = 0;

    // Look up container metadata for a PID. Returns nullopt for
    // host-native processes (no container scope) or when container
    // attribution is disabled.
    [[nodiscard]] virtual std::optional<ContainerInfo>
        resolveContainerForPid(qint32 pid) = 0;
};

} // namespace qiftop::backend
