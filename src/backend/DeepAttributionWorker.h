#pragma once

#include <QObject>

#include "backend/DeepAttribution.h"

namespace qiftop::backend {

// Abstract async deep-pass worker (v0.4 §5). The agent enqueues
// weakly-attributed flows; the worker refines them off the data path and
// streams attribution-only patches back via refined(). Implementations decide
// HOW to refine (re-run the resolver after a cache refresh, enumerate
// threads/fds, request a demand-driven netns scan, ...); the service only
// speaks this interface so it stays transport- and platform-neutral.
//
// `setActive(false)` is the off-mode kill switch: clear pending work and
// suppress further refinements until re-activated. Lives in libqiftop's
// backend layer — Qt Core only, no Widgets, no platform headers.
class DeepAttributionWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~DeepAttributionWorker() override = default;

public slots:
    virtual void enqueue(const QList<qiftop::backend::DeepAttributionRequest> &reqs) = 0;
    virtual void setActive(bool active) = 0;
    virtual void clear() = 0;

    // Re-tune deep-pass budgets (batch size, coalesce interval, queue cap,
    // age-out) on an effective-eagerness change. Default no-op so a worker
    // that doesn't care need not implement it.
    virtual void setTuning(const qiftop::backend::ResolverTuning &) {}

signals:
    // Coalesced batch of attribution-only refinements. The service patches
    // m_last and emits AttributionChanged; it MUST NOT treat these as a rate
    // sample (byte counters are unchanged).
    void refined(const QList<qiftop::backend::DeepAttributionUpdate> &updates);
};

} // namespace qiftop::backend
