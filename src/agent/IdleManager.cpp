#include "IdleManager.h"

#include <QDateTime>
#include <QTimer>

#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"
#include "util/Logging.h"

namespace qiftop::agent {

IdleManager::IdleManager(NetworkMonitor *net, ConnectionMonitor *conn,
                         Config cfg, QObject *parent)
    : QObject(parent)
    , m_net(net)
    , m_conn(conn)
    , m_cfg(cfg)
{
    m_since.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &IdleManager::evaluate);
    m_timer->start();
    applyInterval(effectiveActiveIntervalMs());
}

void IdleManager::noteActivity()
{
    m_since.restart();
    const int target = effectiveActiveIntervalMs();
    if (m_currentMs != target)
        applyInterval(target);
}

void IdleManager::setClientHint(const QString &sender, int ms)
{
    if (sender.isEmpty()) return;
    if (ms <= 0) {
        if (m_hints.remove(sender) > 0) {
            qCInfo(lcVerbose).noquote() << "IdleManager: cleared hint from" << sender;
            evaluate();
        }
        return;
    }
    const int clamped = qMax(ms, m_cfg.minIntervalMs);
    const qint64 exp  = QDateTime::currentMSecsSinceEpoch() + m_cfg.hintTtlMs;
    m_hints.insert(sender, Hint{clamped, exp});
    qCInfo(lcVerbose).noquote()
        << "IdleManager: hint from" << sender << "→" << clamped << "ms (ttl"
        << m_cfg.hintTtlMs/1000 << "s)";
    evaluate();
}

int IdleManager::effectiveActiveIntervalMs()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int best = m_cfg.activeIntervalMs;
    for (auto it = m_hints.begin(); it != m_hints.end(); ) {
        if (it->expiresAtMs <= now) {
            qCInfo(lcVerbose).noquote() << "IdleManager: hint expired for" << it.key();
            it = m_hints.erase(it);
        } else {
            best = qMin(best, it->ms);
            ++it;
        }
    }
    return qMax(best, m_cfg.minIntervalMs);
}

void IdleManager::evaluate()
{
    const qint64 elapsed = m_since.elapsed();
    const int active = effectiveActiveIntervalMs();
    int target;
    if      (elapsed >= m_cfg.idleTimeoutMs)  target = 0;
    else if (elapsed >= m_cfg.slow2WindowMs)  target = 0;
    else if (elapsed >= m_cfg.slow1WindowMs)  target = qMax(m_cfg.slow2IntervalMs, active);
    else if (elapsed >= m_cfg.activeWindowMs) target = qMax(m_cfg.slow1IntervalMs, active);
    else                                       target = active;
    if (target != m_currentMs)
        applyInterval(target);
}

void IdleManager::applyInterval(int ms)
{
    m_currentMs = ms;
    qCInfo(lcVerbose).noquote() << "IdleManager: polling interval ->"
                                << (ms <= 0 ? QStringLiteral("paused")
                                            : QStringLiteral("%1 ms").arg(ms));
    if (m_net)  m_net ->setPollIntervalMs(ms);
    if (m_conn) m_conn->setPollIntervalMs(ms);
}

} // namespace qiftop::agent
