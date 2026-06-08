#include "IdleManager.h"

#include <QDBusConnection>
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
    // Monotonic clock for both since-last-activity and hint expiry. Using
    // QElapsedTimer (which wraps clock_gettime(CLOCK_MONOTONIC) on Linux)
    // means suspend/resume across the deadline doesn't immediately throw
    // every hint into the "expired" bucket the way QDateTime::currentMSec
    // (wall clock, jumps on NTP step too) would.
    m_clock.start();
    m_since.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &IdleManager::evaluate);
    m_timer->start();
    applyInterval(effectiveActiveIntervalMs());
}

void IdleManager::attachBus(const QDBusConnection &bus)
{
    // Subscribe to the canonical NameOwnerChanged signal so that when a
    // peer disconnects we drop its cadence hints immediately instead of
    // waiting up to Config::hintTtlMs. Belt-and-braces: the TTL still
    // governs in unit tests / when the subscription fails for any reason.
    QDBusConnection mut = bus; // QDBusConnection::connect is non-const
    const bool ok = mut.connect(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("NameOwnerChanged"),
        this,
        SLOT(onNameOwnerChanged(QString,QString,QString)));
    if (!ok) {
        qCWarning(lcVerbose) << "IdleManager: failed to subscribe to "
                                "NameOwnerChanged (hints will only expire via TTL)";
    }
}

void IdleManager::onNameOwnerChanged(const QString &name,
                                     const QString &oldOwner,
                                     const QString &newOwner)
{
    // Only the case "owner went away" matters (newOwner empty). The bus
    // sends one event per unique name; we keyed hints on the caller's
    // unique :1.N name in setClientHint, so this is the right key.
    if (!newOwner.isEmpty()) return;
    if (oldOwner.isEmpty())  return;
    if (m_hints.remove(name) > 0) {
        qCInfo(lcVerbose).noquote()
            << "IdleManager: hint dropped — peer disconnected:" << name;
        evaluate();
    }
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
    // Cap the hash so a peer churning unique bus names can't bloat us
    // unboundedly. 64 distinct hinters per agent is well beyond any
    // realistic GUI / scripting scenario. We reject (not evict) so an
    // attacker can't use this as a way to drop legitimate clients' hints.
    static constexpr int kMaxHints = 64;
    if (!m_hints.contains(sender) && m_hints.size() >= kMaxHints) {
        qCWarning(lcVerbose).noquote()
            << "IdleManager: refusing hint from" << sender
            << "— hint table full (" << kMaxHints << ")";
        return;
    }
    const int clamped = qMax(ms, m_cfg.minIntervalMs);
    const qint64 exp  = nowMs() + m_cfg.hintTtlMs;
    m_hints.insert(sender, Hint{clamped, exp});
    qCInfo(lcVerbose).noquote()
        << "IdleManager: hint from" << sender << "→" << clamped << "ms (ttl"
        << m_cfg.hintTtlMs/1000 << "s)";
    evaluate();
}

int IdleManager::effectiveActiveIntervalMs()
{
    const qint64 now = nowMs();
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
    // A zero window means "disable that step", matching the documentation
    // in dist/conf/agent.conf. (Old behaviour: 0 collapsed the comparison
    // to `elapsed >= 0` which is always true, so e.g. idle.timeout_secs=0
    // silently paused polling on the very first tick.)
    int target;
    if      (m_cfg.idleTimeoutMs  > 0 && elapsed >= m_cfg.idleTimeoutMs)  target = 0;
    else if (m_cfg.slow2WindowMs  > 0 && elapsed >= m_cfg.slow2WindowMs)  target = 0;
    else if (m_cfg.slow1WindowMs  > 0 && elapsed >= m_cfg.slow1WindowMs)  target = qMax(m_cfg.slow2IntervalMs, active);
    else if (m_cfg.activeWindowMs > 0 && elapsed >= m_cfg.activeWindowMs) target = qMax(m_cfg.slow1IntervalMs, active);
    else                                                                  target = active;
    if (target != m_currentMs)
        applyInterval(target);
}

void IdleManager::applyInterval(int ms)
{
    const int prev = m_currentMs;
    m_currentMs = ms;
    // Promote the "we just paused / slowed down" log to qWarning so an
    // admin tailing the journal can see when the agent went quiet without
    // having to enable verbose logging first. qCInfo is a macro (not a
    // value-returning function) so we can't conditional-operator the two;
    // branch explicitly.
    const bool degrade = (prev > 0 && (ms <= 0 || ms > prev));
    const QString msg = (ms <= 0) ? QStringLiteral("paused")
                                  : QStringLiteral("%1 ms").arg(ms);
    if (degrade)
        qWarning().noquote() << "IdleManager: polling interval ->" << msg;
    else
        qCInfo(lcVerbose).noquote() << "IdleManager: polling interval ->" << msg;
    if (m_net)  m_net ->setPollIntervalMs(ms);
    if (m_conn) m_conn->setPollIntervalMs(ms);
    emit cadenceChanged(ms);
}

} // namespace qiftop::agent
