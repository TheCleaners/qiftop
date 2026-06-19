#pragma once

#include "backend/DeepAttributionQueue.h"
#include "backend/DeepAttributionWorker.h"
#include "backend/ProcessResolver.h"

class QTimer;

namespace qiftop::backend {

// Concrete deep-pass worker that simply re-runs the wired resolver against
// enqueued flows on a coalescing timer, emitting a refinement whenever the
// retry finds attribution the cheap snapshot pass missed (a PID, container, or
// chain that wasn't there before). The win is timing, not new kernel work: a
// flow whose socket hadn't landed in the resolver's cache yet gets a second
// chance once the resolver's periodic refresh catches up.
//
// Runs on whatever thread it lives on (the agent keeps it on the main thread
// alongside ConnectionsService), so resolver calls stay serialised with the
// data path — no concurrent resolver access. The genuinely expensive recovery
// (per-PID thread/fd enumeration, demand-driven netns scans) is a separate
// follow-up worker; this one deliberately reuses only the cheap, cache-backed
// attributeFlows() path so it can never stall the event loop.
class ResolverDeepWorker : public DeepAttributionWorker {
    Q_OBJECT

public:
    explicit ResolverDeepWorker(QObject *parent = nullptr);

    // The resolver to retry against. Must be the same instance (or at least a
    // thread-compatible one) the service uses; calls happen on this worker's
    // thread. Null disables refinement (enqueued work ages out harmlessly).
    void setResolver(ProcessResolver *resolver) { m_resolver = resolver; }

    void setTuning(const ResolverTuning &tuning) override;

    void enqueue(const QList<qiftop::backend::DeepAttributionRequest> &reqs) override;
    void setActive(bool active) override;
    void clear() override;

    [[nodiscard]] int pendingCount() const { return m_queue.size(); }
    [[nodiscard]] bool active() const { return m_active; }

private slots:
    void process();

private:
    void ensureRunning();

    DeepAttributionQueue       m_queue;
    ProcessResolver           *m_resolver = nullptr;
    QTimer                    *m_timer = nullptr;
    bool                       m_active = true;
    int                        m_batchMax = 256;
    int                        m_maxAttempts = 12;
    int                        m_coalesceMs = 100;
    bool                       m_demandNetnsScan = false;
};

} // namespace qiftop::backend
