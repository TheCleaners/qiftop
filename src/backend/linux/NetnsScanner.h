#pragma once

#include <QHash>
#include <QStringList>

#include <atomic>
#include <mutex>

#include "backend/ProcessResolver.h"

class QThread;

namespace qiftop::backend::linuximpl {

class NetnsScannerWorker;

// Cross-network-namespace socket attribution resolver.
//
// PROBLEM
//   The agent runs in the host's netns and dumps host conntrack. For
//   bridged / macvlan / IPv6 / `--net=container` Docker setups (i.e.
//   anything not NAT'd by the host), a flow's local 4-tuple matches a
//   socket that lives INSIDE a container's netns — invisible to a
//   netlink dump issued from the host netns. This resolver enumerates
//   non-host netnses and dumps each one, so resolvePid()/enrichPid() can attribute
//   those flows too.
//
// THREADING (read AGENTS.md §8a rule 5 before touching this)
//   setns(2) with CLONE_NEWNET mutates the CALLING THREAD's netns. We
//   therefore run all the netns dancing on a dedicated worker thread
//   that is solely owned by this resolver — never share that thread
//   with anything else (its netns identity is constantly mutating).
//
//   The worker:
//     1. Snapshots its own anchor netns fd at startup.
//     2. Every refresh tick: walks /proc/<pid>/ns/net once, grouping pids
//        by netns inode, dedupes inodes, skips the host inode.
//     3. For each non-host inode: opens that netns fd; saves anchor;
//        setns(target). Wrapped in a scope guard that ALWAYS setns
//        back to anchor on exit, qFatal()ing the agent on restore
//        failure (running future dumps in a stranger's netns would
//        corrupt every subsequent attribution).
//     4. Opens a *fresh* NETLINK_SOCK_DIAG socket inside the target
//        netns (netlink sockets are bound to the netns they were
//        created in for life — you cannot reuse an anchor-netns
//        netlink fd inside a different netns).
//     5. Dumps TCPv4/v6, UDPv4/v6. Scans only the pids already grouped
//        for that netns to map socket inodes back to pids.
//     6. Publishes the merged (4-tuple → pid) map atomically.
//
//   resolvePid()/enrichPid() run on the agent data thread and only read the
//   published map under a mutex — never touches setns.
//
// FAILURE MODES
//   - No CAP_SYS_ADMIN → initialize() returns false; capabilities() = {}.
//   - /proc/<pid>/ns/net missing or unreadable → skip silently.
//   - setns(target) fails (netns destroyed mid-walk) → skip that ns.
//   - setns(anchor) fails on restore → qFatal (catastrophic).
//   - Per-netns sock_diag dump fails → log once per cycle, continue.
class NetnsScanner final : public ProcessResolver {
public:
    explicit NetnsScanner(const ResolverTuning &tuning = balancedResolverTuning());
    ~NetnsScanner() override;

    // Probes /proc/self/ns/net + an unshare(CLONE_NEWNET)-style read.
    // We don't actually unshare — we just verify the fd is openable.
    // Returns false (with a single qCWarning) on missing privilege.
    bool initialize() override;

    [[nodiscard]] QStringList capabilities() const override;

    // Live re-tune of the cross-netns scan cadence. The refresh interval is
    // an atomic int; the worker thread re-reads it at the top of each tick
    // and adjusts its own QTimer (QTimer is not thread-safe, so we never
    // touch it from the agent main thread). Clamps to the netns floor.
    void setTuning(const ResolverTuning &tuning) override;

    // Demand-driven early scan (v0.4 §5). Posts a queued tick to the worker
    // thread so an unresolved container flow's netns is swept now instead of
    // at the next periodic refresh. Rate-limited internally (≈ netnsRefresh/4,
    // floor 250 ms) so a burst of misses can't storm the scanner. Non-blocking.
    void requestDeepScan() override;

    [[nodiscard]] qint32 resolvePid(const Connection &flow) override;

    [[nodiscard]] std::optional<ProcessInfo>
        enrichPid(qint32 pid) override;

    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32) override { return std::nullopt; }

private:
    bool                              m_ready = false;
    std::atomic<bool>                 m_stop{false};
    QThread                          *m_thread = nullptr;
    NetnsScannerWorker               *m_worker = nullptr;
    // Cross-netns scan cadence (ms). Atomic because the agent main thread
    // re-tunes it via setTuning() while the worker thread reads it each tick.
    std::atomic<int>                  m_refreshIntervalMs{5000};
    // Monotonic ms of the last demand scan, for rate-limiting requestDeepScan.
    std::atomic<qint64>               m_lastDemandMs{0};

    // Published cross-netns maps; mutated by worker, read by data thread.
    mutable std::mutex                m_mu;
    QHash<QByteArray, quint64>        m_keyToInode;
    struct PidStamp { qint32 pid; quint64 startTime; };
    QHash<quint64, PidStamp>          m_inodeToPid;
    QHash<qint32, quint64>            m_pidToStartTime;

    friend class NetnsScannerWorker;
};

} // namespace qiftop::backend::linuximpl
