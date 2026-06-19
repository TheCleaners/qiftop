#pragma once

#include <QHostAddress>
#include <QList>
#include <QtGlobal>

#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

// Async deep-pass attribution types (v0.4 §5). The cheap snapshot pass in
// ConnectionsService stays synchronous and bounded; flows it couldn't fully
// attribute are enqueued here for a deep worker to refine off the data path.
// Refinements stream back as attribution-only patches (no rate resample).
//
// Widgets-free, lives in libqiftop's backend layer so the agent and any
// future in-process consumer share one vocabulary. No platform headers.

namespace qiftop::backend {

// Stable identity for matching a deferred refinement back to a live flow.
// Mirrors the identifying fields already shipped in ConnectionDto and,
// crucially, includes Direction because Connection::key() treats direction
// as part of identity (so aggregated inbound/outbound rows for the same peer
// never collide).
struct AttributionFlowKey {
    L4Proto      proto = L4Proto::Unknown;
    QHostAddress localAddress;
    quint16      localPort = 0;
    QHostAddress remoteAddress;
    quint16      remotePort = 0;
    quint32      ifIndex = 0;
    Direction    direction = Direction::Unknown;

    friend bool operator==(const AttributionFlowKey &,
                           const AttributionFlowKey &) = default;
};

[[nodiscard]] inline AttributionFlowKey keyOf(const Connection &c)
{
    return AttributionFlowKey{
        .proto         = c.proto,
        .localAddress  = c.local.address,
        .localPort     = c.local.port,
        .remoteAddress = c.remote.address,
        .remotePort    = c.remote.port,
        .ifIndex       = c.ifIndex,
        .direction     = c.direction,
    };
}

inline size_t qHash(const AttributionFlowKey &k, size_t seed = 0) noexcept
{
    return qHashMulti(seed,
                      static_cast<quint8>(k.proto),
                      k.localAddress,
                      k.localPort,
                      k.remoteAddress,
                      k.remotePort,
                      k.ifIndex,
                      static_cast<quint8>(k.direction));
}

// One queued unit of deep work. Carries a cheap-fields snapshot of the flow
// (NEVER the expensive exe/cmdline — those stay behind GetProcessDetails) plus
// the snapshot generation it came from, so a stale refinement can be dropped.
struct DeepAttributionRequest {
    AttributionFlowKey key;
    Connection         flow;              // cheap fields only
    quint64            generation = 0;    // ConnectionsService snapshot generation
    quint64            monotonicMsQueued = 0;
    bool               wantContainerChain = false;

    // How many deep ticks this request has survived without being resolved.
    // The queue ages out requests that never converge so a permanently
    // unattributable flow (forwarded/NAT) doesn't churn forever.
    int                attempts = 0;
};

// One refinement result. Applied as an attribution-only patch to the matching
// live flow; the service merges it into m_last and emits AttributionChanged.
struct DeepAttributionUpdate {
    AttributionFlowKey   key;
    quint64              generation = 0;
    ProcessInfo          process;
    ContainerInfo        container;
    QList<ContainerInfo> containerChain;
    AttributionReason    reason = AttributionReason::NoLocalSocket;
};

} // namespace qiftop::backend

// Declared here (before DeepAttributionWorker's refined() signal uses the
// type) so moc doesn't instantiate the metatype ahead of the declaration.
Q_DECLARE_METATYPE(qiftop::backend::DeepAttributionRequest)
Q_DECLARE_METATYPE(qiftop::backend::DeepAttributionUpdate)
