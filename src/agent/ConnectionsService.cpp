#include "ConnectionsService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include <algorithm>
#include <limits>

#include "IdleManager.h"
#include "Attribution.h"
#include "backend/ConnectionMonitor.h"
#include "backend/PlatformInfo.h"
#include "util/ConnectionHeuristics.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ProcDetails.h"
#endif

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
    // See ConnectionsService::SetDesiredIntervalMs — only count accepted
    // hints as activity (see commit message for rationale).
    if (m_idle->setClientHint(sender, static_cast<int>(intervalMs)))
        m_idle->noteActivity();
}

dbus::ProcessDetailsDto ConnectionsService::GetProcessDetails(uint pid)
{
    if (m_idle) m_idle->noteActivity();
    dbus::ProcessDetailsDto out;
    if (pid == 0 || pid > quint32(std::numeric_limits<qint32>::max()))
        return out;

#ifdef BACKEND_LINUX
    const auto d = backend::linux_::readProcessDetails(static_cast<qint32>(pid));
    if (!d.valid) return out;
    out.pid              = quint32(d.pid);
    out.uid              = d.uid;
    out.comm             = d.comm;
    out.exe              = d.exe;
    out.cmdline          = d.cmdline;
    out.cwd              = d.cwd;
    out.startTimeJiffies = d.startTimeJiffies;
#else
    Q_UNUSED(pid);
#endif
    return out;
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

    // Populate process + container attribution from the wired resolver.
    // No-op when resolver is null or returns nothing useful. PID/comm
    // memoised internally so a single container hosting many flows
    // costs O(unique-pids), not O(flows). See agent::attributeFlows.
    attributeFlows(kept, m_resolver, AttributionOptions{m_wantContainerChain});

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
