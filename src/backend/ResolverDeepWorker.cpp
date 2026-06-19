#include "backend/ResolverDeepWorker.h"

#include <QTimer>

#include "backend/Attribution.h"

namespace qiftop::backend {

ResolverDeepWorker::ResolverDeepWorker(QObject *parent)
    : DeepAttributionWorker(parent)
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &ResolverDeepWorker::process);
}

void ResolverDeepWorker::setTuning(const ResolverTuning &tuning)
{
    m_batchMax    = tuning.deepBatchMax    > 0 ? tuning.deepBatchMax    : 1;
    m_maxAttempts = tuning.deepMaxAttempts > 0 ? tuning.deepMaxAttempts : 1;
    m_coalesceMs  = tuning.deepCoalesceMs  > 0 ? tuning.deepCoalesceMs  : 50;
    m_queue.setCapacity(tuning.deepQueueMax);
    if (m_timer->isActive())
        m_timer->setInterval(m_coalesceMs);
}

void ResolverDeepWorker::enqueue(const QList<DeepAttributionRequest> &reqs)
{
    if (!m_active || reqs.isEmpty())
        return;
    m_queue.enqueue(reqs);
    ensureRunning();
}

void ResolverDeepWorker::setActive(bool active)
{
    m_active = active;
    if (!active) {
        m_queue.clear();
        m_timer->stop();
    }
}

void ResolverDeepWorker::clear()
{
    m_queue.clear();
    m_timer->stop();
}

void ResolverDeepWorker::ensureRunning()
{
    if (!m_timer->isActive() && !m_queue.isEmpty())
        m_timer->start(m_coalesceMs);
}

void ResolverDeepWorker::process()
{
    if (!m_active || m_resolver == nullptr) {
        m_queue.clear();
        m_timer->stop();
        return;
    }

    const QList<DeepAttributionRequest> batch = m_queue.dequeue(m_batchMax);
    QList<DeepAttributionUpdate> updates;
    QList<DeepAttributionRequest> retry;

    for (const DeepAttributionRequest &req : batch) {
        // Re-run the cheap, cache-backed attribution against a private copy of
        // the flow. The resolver re-checks PID starttime internally, so a
        // recycled PID can't leak the previous owner's attribution.
        QList<Connection> one{req.flow};
        attributeFlows(one, m_resolver,
                       AttributionOptions{.wantContainerChain = req.wantContainerChain});
        const Connection &got = one.front();

        const bool gotProcess   = got.process.valid()   && !req.flow.process.valid();
        const bool gotContainer = got.container.valid() && !req.flow.container.valid();
        const bool gotChain     = !got.containerChain.isEmpty()
                               &&  req.flow.containerChain.isEmpty();

        if (gotProcess || gotContainer || gotChain) {
            DeepAttributionUpdate u;
            u.key            = req.key;
            u.generation     = req.generation;
            u.process        = got.process;
            u.container      = got.container;
            u.containerChain = got.containerChain;
            u.reason         = got.process.valid() ? AttributionReason::Resolved
                                                   : req.flow.reason;
            updates.append(u);
            continue;
        }

        // No improvement yet — retry on a later tick until the flow ages out.
        // This is what lets a flow whose socket lands in the resolver cache a
        // moment after the snapshot eventually attribute.
        if (req.attempts + 1 < m_maxAttempts) {
            DeepAttributionRequest again = req;
            ++again.attempts;
            retry.append(again);
        }
    }

    if (!retry.isEmpty())
        m_queue.enqueue(retry);

    if (!updates.isEmpty())
        emit refined(updates);

    if (m_queue.isEmpty())
        m_timer->stop();
}

} // namespace qiftop::backend
