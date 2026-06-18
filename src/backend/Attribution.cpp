#include "Attribution.h"

#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

// Header-only, pure-Qt /proc/<pid>/stat starttime reader. Lives under
// backend/linux/ but pulls in no Linux UAPI headers, so including it
// here keeps agent/ compilable on any Qt 6 target (on non-Linux the
// read simply fails and the memo degrades to pid-only keying).
#include "backend/linux/ProcSnapshot.h"

#include <QHash>

#include <utility>

namespace qiftop::backend {

namespace {

// Memo key: (pid, starttime). The starttime disambiguates a PID the
// kernel recycled between two flows of the SAME snapshot pass — the
// recycled pid gets a fresh resolver lookup instead of the previous
// owner's cached attribution (AGENTS.md §8a rule 2).
using PidKey = std::pair<qint32, quint64>;

} // namespace

void attributeFlows(QList<Connection> &flows,
                    ProcessResolver *resolver,
                    const AttributionOptions &opts)
{
    if (!resolver) return;

    const auto startTimeOf = [&opts](qint32 pid) -> quint64 {
        if (opts.startTimeForPid) return opts.startTimeForPid(pid);
        return linuximpl::procsnap::pidStartTime(pid).value_or(0);
    };

    // Per-tick memoisation: many flows share a PID (every conn of one
    // server process) and a cgroup (every conn of one container). PID
    // resolution remains per-flow because the 4-tuple is unique, but
    // /proc-backed process enrichment and container lookup are per-PID
    // — keyed by (pid, starttime) to stay PID-reuse safe.
    QHash<PidKey, ProcessInfo>            processByPid;
    QHash<PidKey, ContainerInfo>          containerByPid;
    QHash<PidKey, QList<ContainerInfo>>   chainByPid;

    for (auto &c : flows) {
        // 1. Process attribution. resolvePid is the cheap socket-table
        //    lookup; enrichPid does the expensive /proc reads and is
        //    memoised per tick by (pid, starttime).
        const qint32 pid = resolver->resolvePid(c);
        if (pid <= 0) continue;
        const PidKey key{pid, startTimeOf(pid)};
        if (auto it = processByPid.constFind(key); it != processByPid.constEnd()) {
            c.process = it.value();
        } else {
            ProcessInfo info{};
            if (auto pi = resolver->enrichPid(pid); pi && pi->valid()) {
                info = *pi;
            }
            processByPid.insert(key, info);
            c.process = info;
        }
        if (!c.process.valid()) continue;

        // 2. Container attribution. Memoised by (pid, starttime) so a
        //    single container hosting 200 connections costs one lookup.
        if (auto it = containerByPid.constFind(key); it != containerByPid.constEnd()) {
            c.container = it.value();
        } else {
            ContainerInfo ci{};
            if (auto found = resolver->resolveContainerForPid(pid)) ci = *found;
            containerByPid.insert(key, ci);
            c.container = ci;
        }

        // 3. Full nesting chain. Only when the agent advertises
        //    container-chain-wire — otherwise keep `containerChain`
        //    empty so the wire signal matches the capability bit
        //    (clients gate on token presence).
        if (opts.wantContainerChain) {
            if (auto it = chainByPid.constFind(key); it != chainByPid.constEnd()) {
                c.containerChain = it.value();
            } else {
                auto chain = resolver->resolveContainerChainForPid(pid);
                chainByPid.insert(key, chain);
                c.containerChain = std::move(chain);
            }
        }
    }
}

} // namespace qiftop::backend
