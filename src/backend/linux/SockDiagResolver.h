#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QStringList>

#include <memory>
#include <mutex>

#include "backend/ProcessResolver.h"

namespace qiftop::backend::linuximpl {

// Linux per-flow process attribution resolver.
//
// DATA SOURCES
//   * NETLINK_SOCK_DIAG (inet_diag) — maps live TCP/UDP sockets (v4 + v6)
//     to their kernel inode + owning UID. No privileges required for own
//     sockets; root sees all.
//   * /proc/<pid>/fd/* — readlink targets like "socket:[<inode>]" give the
//     inode → PID mapping. /proc/<pid>/comm and /proc/<pid>/status give
//     the process name and uid.
//
// SCOPE
//   v0.2 step 2 ships HOST netns only. Cross-namespace scanning lands in
//   step 4 (NetnsScanner composes this resolver into each /proc/<pid>/ns/net
//   target). A flow originating in a Docker container's netns will return
//   nullopt from this resolver alone — that's expected.
//
// CACHING
//   Both the socket table and the inode→PID map are rebuilt at most once
//   per configured cache TTL (default 1s) to amortise across the typical 20–500
//   flow lookups that arrive together when ConntrackMonitor ticks.
//   resolvePid() does only cached table lookups; enrichPid() performs
//   /proc metadata reads and is intended to be memoised per unique PID by
//   the caller (backend::attributeFlows does this for each snapshot).
//
// THREADING
//   resolvePid()/enrichPid() are called from the agent thread (or, future: the
//   ConntrackMonitor worker thread). Internal caches are guarded by a
//   single mutex — contention is negligible at our call rate.
class SockDiagResolver final : public ProcessResolver {
public:
    explicit SockDiagResolver(const ResolverTuning &tuning = balancedResolverTuning());
    ~SockDiagResolver() override;

    // Opens the NETLINK_SOCK_DIAG socket. Returns true if the socket
    // opened AND a probe dump succeeded; false (with a logged warning)
    // if the running kernel lacks sock_diag (extremely rare on anything
    // post-2012) or the agent lacks permission. On failure resolvePid
    // always returns 0 and capabilities() is empty.
    bool initialize() override;

    [[nodiscard]] QStringList capabilities() const override;

    // Live re-tune of the socket-table / proc-walk refresh cadence.
    // Thread-safe: takes the same Impl mutex the poll loop reads
    // cacheTtlMs under. Clamps to the resolver's existing floor.
    void setTuning(const ResolverTuning &tuning) override;

    [[nodiscard]] qint32 resolvePid(const Connection &flow) override;

    [[nodiscard]] std::optional<ProcessInfo>
        enrichPid(qint32 pid) override;

    // Step 2 doesn't ship container attribution; the future cgroup
    // classifier (step 3) is what populates this.
    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32 /*pid*/) override { return std::nullopt; }

private:
    // Forward-declared impl owns the netlink fd + caches; keeps the
    // public header free of Linux UAPI types.
    struct Impl;
    std::unique_ptr<Impl> m_d;
};

} // namespace qiftop::backend::linuximpl
