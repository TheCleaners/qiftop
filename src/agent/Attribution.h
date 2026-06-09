#pragma once

// Server-side attribution glue: enrich a snapshot of Connection rows
// with process + container metadata sourced from a ProcessResolver.
//
// Extracted as a free function (not a member of ConnectionsService) so
// it can be unit-tested with a FakeProcessResolver — the service slot
// is signal-driven and hard to drive directly.
//
// Threading: callers are expected to call this from the same thread
// that constructs the resolver and owns the slot (the agent main
// thread). ProcessResolver implementations are documented as
// reentrant-safe across resolvePid / enrichPid / resolveContainerForPid /
// resolveContainerChainForPid (ProcessResolver.h §THREADING), so this
// helper makes no extra synchronisation.

#include <QList>

struct Connection;

namespace qiftop::backend { class ProcessResolver; }

namespace qiftop::agent {

struct AttributionOptions {
    // When true, callers expect the resolver to advertise the
    // `container-chain` capability and we'll call
    // resolveContainerChainForPid() — otherwise we only call
    // resolveContainerForPid() (which the default impl wraps into a
    // single-entry chain anyway, but we want to keep the wire honest:
    // when the agent doesn't advertise `container-chain-wire`, every
    // flow's containerChain stays empty).
    bool wantContainerChain = false;
};

// Attribute `flows` in-place. Sets `c.process`, `c.container`, and
// (when wantContainerChain) `c.containerChain` on each entry whose
// resolver lookup succeeded. Entries with no PID, an inert resolver,
// or a host-native PID keep their default-constructed attribution
// (pid=0, runtime empty, chain empty).
//
// Process enrichment is split into cheap per-flow PID resolution and
// memoised per-PID metadata reads so /proc work stays O(unique-pids).
//
// Safe to call with resolver==nullptr: no-op (every flow keeps zero
// attribution).
void attributeFlows(QList<Connection> &flows,
                    backend::ProcessResolver *resolver,
                    const AttributionOptions &opts = {});

} // namespace qiftop::agent
