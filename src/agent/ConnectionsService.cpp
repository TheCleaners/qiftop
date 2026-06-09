#include "ConnectionsService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include <algorithm>

#include "IdleManager.h"
#include "backend/ConnectionMonitor.h"
#include "backend/PlatformInfo.h"
#include "util/ConnectionHeuristics.h"

namespace qiftop::agent {

namespace {

// Snapshot of "what counts as this host" for direction inference.
// Refreshed every tick because addresses can come and go (DHCP renew,
// VPN up/down, container creation).
struct HostContext {
    QSet<QHostAddress> localAddrs;
    QSet<QHostAddress> loopbackAddrs;
    quint16            ephemeralLow  = 32768;
    quint16            ephemeralHigh = 60999;
};

HostContext gatherHostContext()
{
    HostContext ctx;
    ctx.localAddrs    = qiftop::platform::localAddresses();
    ctx.loopbackAddrs = qiftop::platform::loopbackAddresses();
    const auto [lo, hi] = qiftop::platform::ephemeralPortRange();
    ctx.ephemeralLow  = lo;
    ctx.ephemeralHigh = hi;
    return ctx;
}

} // namespace

ConnectionsService::ConnectionsService(ConnectionMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    m_clock.start();
    connect(m_monitor, &ConnectionMonitor::connectionsUpdated,
            this,      &ConnectionsService::onConnectionsUpdated);
    connect(m_monitor, &ConnectionMonitor::permissionDenied,
            this,      &ConnectionsService::onPermissionDenied);
    connect(m_monitor, &ConnectionMonitor::accountingUnavailable,
            this,      &ConnectionsService::onAccountingUnavailable);
}

dbus::ConnectionDtoList ConnectionsService::GetConnections()
{
    if (m_idle) m_idle->noteActivity();
    return m_last;
}

void ConnectionsService::SetDesiredIntervalMs(uint intervalMs)
{
    if (!m_idle) return;
    const QString sender = calledFromDBus() ? message().service() : QString();
    // Only count this call as activity if the hint was actually accepted.
    // Otherwise a rejected peer (hint table full, empty sender) could keep
    // the agent out of idle by hammering this method even though we did
    // no real work.
    if (m_idle->setClientHint(sender, static_cast<int>(intervalMs)))
        m_idle->noteActivity();
}

void ConnectionsService::onConnectionsUpdated(const QList<Connection> &conns)
{
    // Cap the per-tick snapshot size. On a busy router the conntrack
    // table can hold 100 k+ flows; serialising that into a multi-MB DBus
    // message every tick costs both sides real CPU and memory (and m_last
    // pins the high-water mark for the life of the process). Keep the
    // top N by total bytes — those are what the user actually wants to
    // see in a "top talkers" tool — and log when we truncate.
    static constexpr int kMaxConnections = 4096;

    QList<Connection> kept = conns;
    if (kept.size() > kMaxConnections) {
        std::partial_sort(kept.begin(),
                          kept.begin() + kMaxConnections,
                          kept.end(),
                          [](const Connection &a, const Connection &b) {
                              return (a.rxBytes + a.txBytes) > (b.rxBytes + b.txBytes);
                          });
        kept.resize(kMaxConnections);
        // qWarning, not qCInfo, because it means the user is losing data.
        qWarning().noquote()
            << "ConnectionsService: capping snapshot at" << kMaxConnections
            << "of" << conns.size() << "flows (kept top talkers by bytes)";
    }

    // Populate Direction server-side so non-Qt libqiftop consumers don't
    // have to reimplement the heuristic. Done AFTER truncation so we
    // only pay for flows we'll actually ship.
    const auto ctx = gatherHostContext();
    for (auto &c : kept) {
        c.direction = heuristics::inferDirection(
            c, ctx.localAddrs, ctx.loopbackAddrs,
            ctx.ephemeralLow, ctx.ephemeralHigh);
    }

    m_last = dbus::toDtos(kept);
    emit ConnectionsChanged(static_cast<qulonglong>(m_clock.elapsed()), m_last);
}

void ConnectionsService::onPermissionDenied(const QString &detail)
{
    emit PermissionDenied(detail);
}

void ConnectionsService::onAccountingUnavailable(const QString &detail)
{
    if (m_accountingEnabled) {
        m_accountingEnabled = false;
        emit AccountingChanged(false);
    }
    // detail is informational only; PermissionDenied is the actionable signal.
    Q_UNUSED(detail);
}

} // namespace qiftop::agent
