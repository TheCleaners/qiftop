#include "AttributionHintManager.h"

#include <QDBusConnection>
#include <QTimer>

#include <algorithm>

#include "util/Logging.h"

namespace qiftop::agent {

using Eagerness = backend::AttributionEagerness;

namespace {
// Most-eager-wins ordering. The enum values already sort Off(0) <
// Balanced(1) < Eager(2), so a plain numeric max is the answer.
[[nodiscard]] Eagerness moreEager(Eagerness a, Eagerness b)
{
    return std::max(a, b);
}
} // namespace

AttributionHintManager::AttributionHintManager(Eagerness configMode,
                                               int hintTtlMs, QObject *parent)
    : QObject(parent)
    , m_configMode(configMode)
    , m_hintTtlMs(qMax(1, hintTtlMs))
    , m_lastEmitted(configMode)
{
    // Monotonic clock for hint expiry — same rationale as IdleManager
    // (suspend/resume + NTP steps must not nuke every hint at once).
    m_clock.start();

    // Periodic prune so an expiring hint that LOWERS the effective mode
    // still fires effectiveModeChanged() without needing a fresh inbound
    // call. Half-TTL cadence matches the recommended client heartbeat.
    m_pruneTimer = new QTimer(this);
    m_pruneTimer->setInterval(qMax(1, m_hintTtlMs / 2));
    connect(m_pruneTimer, &QTimer::timeout, this, &AttributionHintManager::evaluate);
    m_pruneTimer->start();
}

void AttributionHintManager::attachBus(const QDBusConnection &bus)
{
    QDBusConnection mut = bus; // QDBusConnection::connect is non-const
    const bool ok = mut.connect(
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("NameOwnerChanged"),
        this,
        SLOT(onNameOwnerChanged(QString,QString,QString)));
    if (!ok) {
        qCWarning(lcVerbose) << "AttributionHintManager: failed to subscribe to "
                                "NameOwnerChanged (hints will only expire via TTL)";
    }
}

void AttributionHintManager::onNameOwnerChanged(const QString &name,
                                                const QString &oldOwner,
                                                const QString &newOwner)
{
    // "owner went away": newOwner empty, oldOwner non-empty. Hints are keyed
    // on the caller's unique :1.N name, which is exactly `name` here.
    if (!newOwner.isEmpty()) return;
    if (oldOwner.isEmpty())  return;
    if (m_hints.remove(name) > 0) {
        qCInfo(lcVerbose).noquote()
            << "AttributionHintManager: hint dropped — peer disconnected:" << name;
        evaluate();
    }
}

backend::AttributionEagerness AttributionHintManager::effectiveMode() const
{
    // Rule 1: config off is an uncancellable kill switch.
    if (m_configMode == Eagerness::Off) return Eagerness::Off;

    // Rule 2: if any live hint exists, the most eager of them wins (the
    // config default is intentionally NOT folded in, so a lone client can
    // lower a config `eager` to `balanced`).
    const qint64 now = nowMs();
    bool any = false;
    Eagerness best = Eagerness::Off;
    for (auto it = m_hints.constBegin(); it != m_hints.constEnd(); ++it) {
        if (it->expiresAtMs <= now) continue; // skip (const: don't erase)
        best = any ? moreEager(best, it->mode) : it->mode;
        any = true;
    }
    if (any) return best;

    // Rule 3: no live hints → config default.
    return m_configMode;
}

bool AttributionHintManager::setHint(const QString &sender, Eagerness mode)
{
    if (sender.isEmpty()) return false;
    // Cap the table so a peer churning unique bus names can't bloat it.
    // 64 distinct hinters is well beyond any realistic scenario. Reject
    // (don't evict) so an attacker can't drop legitimate clients' hints.
    static constexpr int kMaxHints = 64;
    if (!m_hints.contains(sender) && m_hints.size() >= kMaxHints) {
        qCWarning(lcVerbose).noquote()
            << "AttributionHintManager: refusing hint from" << sender
            << "— hint table full (" << kMaxHints << ")";
        return false;
    }
    const qint64 exp = nowMs() + m_hintTtlMs;
    m_hints.insert(sender, Hint{mode, exp});
    qCInfo(lcVerbose).noquote()
        << "AttributionHintManager: hint from" << sender << "→"
        << backend::eagernessToString(mode) << "(ttl" << m_hintTtlMs / 1000 << "s)";
    evaluate();
    return true;
}

bool AttributionHintManager::clearHint(const QString &sender)
{
    if (sender.isEmpty()) return false;
    if (m_hints.remove(sender) > 0) {
        qCInfo(lcVerbose).noquote()
            << "AttributionHintManager: cleared hint from" << sender;
        evaluate();
    }
    return true;
}

void AttributionHintManager::evaluate()
{
    // Prune expired hints first so the table doesn't grow unbounded and the
    // emitted transition reflects reality.
    const qint64 now = nowMs();
    for (auto it = m_hints.begin(); it != m_hints.end(); ) {
        if (it->expiresAtMs <= now) {
            qCInfo(lcVerbose).noquote()
                << "AttributionHintManager: hint expired for" << it.key();
            it = m_hints.erase(it);
        } else {
            ++it;
        }
    }
    const Eagerness eff = effectiveMode();
    if (eff != m_lastEmitted) {
        m_lastEmitted = eff;
        emit effectiveModeChanged(eff);
    }
}

} // namespace qiftop::agent
