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
    // server process) and a cgroup (every conn of one container). The
    // resolver implementations cache too, but the cache lookup is not
    // free — a local QHash hop is cheaper still and bounds the cost at
    // O(unique-pids) rather than O(flows).
    QHash<qint32, backend::ContainerInfo>          containerByPid;
    QHash<qint32, QList<backend::ContainerInfo>>   chainByPid;

    for (auto &c : flows) {
        // 1. Process attribution. resolveFlow may fail (forwarded flows,
        //    closed sockets, netns we don't scan) — leave defaults.
        const auto pi = resolver->resolveFlow(c);
        if (!pi || !pi->valid()) continue;
        c.process = *pi;

        // 2. Container attribution. Memoised by PID so a single
        //    container hosting 200 connections costs one lookup.
        const qint32 pid = c.process.pid;
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
