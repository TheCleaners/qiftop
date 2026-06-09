#include "Attribution.h"

#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

#include <QHash>

namespace qiftop::agent {

void attributeFlows(QList<Connection> &flows,
                    backend::ProcessResolver *resolver,
                    const AttributionOptions &opts)
{
    if (!resolver) return;

    // Per-tick memoisation: many flows share a PID (every conn of one
    // server process) and a cgroup (every conn of one container). PID
    // resolution remains per-flow because the 4-tuple is unique, but
    // /proc-backed process enrichment and container lookup are per-PID.
    QHash<qint32, backend::ProcessInfo>            processByPid;
    QHash<qint32, backend::ContainerInfo>          containerByPid;
    QHash<qint32, QList<backend::ContainerInfo>>   chainByPid;

    for (auto &c : flows) {
        // 1. Process attribution. resolvePid is the cheap socket-table
        //    lookup; enrichPid does the expensive /proc reads and is
        //    memoised per tick by PID.
        const qint32 pid = resolver->resolvePid(c);
        if (pid <= 0) continue;
        if (auto it = processByPid.constFind(pid); it != processByPid.constEnd()) {
            c.process = it.value();
        } else {
            backend::ProcessInfo info{};
            if (auto pi = resolver->enrichPid(pid); pi && pi->valid()) {
                info = *pi;
            }
            processByPid.insert(pid, info);
            c.process = info;
        }
        if (!c.process.valid()) continue;

        // 2. Container attribution. Memoised by PID so a single
        //    container hosting 200 connections costs one lookup.
        if (auto it = containerByPid.constFind(pid); it != containerByPid.constEnd()) {
            c.container = it.value();
        } else {
            backend::ContainerInfo ci{};
            if (auto found = resolver->resolveContainerForPid(pid)) ci = *found;
            containerByPid.insert(pid, ci);
            c.container = ci;
        }

        // 3. Full nesting chain. Only when the agent advertises
        //    container-chain-wire — otherwise keep `containerChain`
        //    empty so the wire signal matches the capability bit
        //    (clients gate on token presence).
        if (opts.wantContainerChain) {
            if (auto it = chainByPid.constFind(pid); it != chainByPid.constEnd()) {
                c.containerChain = it.value();
            } else {
                auto chain = resolver->resolveContainerChainForPid(pid);
                chainByPid.insert(pid, chain);
                c.containerChain = std::move(chain);
            }
        }
    }
}

} // namespace qiftop::agent
