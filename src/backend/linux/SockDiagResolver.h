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
//   per kCacheTtlMs (default 1s) to amortise across the typical 20–500
//   flow lookups that arrive together when ConntrackMonitor ticks. The
//   sock_diag dump is cheap (~ms even on busy hosts); the /proc walk is
//   the expensive bit and is gated by a dirty flag — we only walk /proc
//   when we get a socket-table hit for an inode we don't yet know.
//
// THREADING
//   resolveFlow() is called from the agent thread (or, future: the
//   ConntrackMonitor worker thread). Internal caches are guarded by a
//   single mutex — contention is negligible at our call rate.
class SockDiagResolver final : public ProcessResolver {
public:
    SockDiagResolver();
    ~SockDiagResolver() override;

    // Opens the NETLINK_SOCK_DIAG socket. Returns true if the socket
    // opened AND a probe dump succeeded; false (with a logged warning)
    // if the running kernel lacks sock_diag (extremely rare on anything
    // post-2012) or the agent lacks permission. On failure resolveFlow
    // always returns nullopt and capabilities() is empty.
    bool initialize() override;

    [[nodiscard]] QStringList capabilities() const override;

    [[nodiscard]] std::optional<ProcessInfo>
        resolveFlow(const Connection &flow) override;

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
