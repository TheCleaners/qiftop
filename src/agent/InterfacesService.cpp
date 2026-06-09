#include "InterfacesService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include "IdleManager.h"
#include "backend/NetworkMonitor.h"
#include "backend/ProcessResolver.h"

namespace qiftop::agent {

namespace {
// Bump alongside CMake `project(... VERSION ...)`. Read by clients via
// the standard Properties.Get to display "Connected to qiftop-agent vX"
// and gate optional feature use. Breaking wire changes still require a
// NetworkAgent2 interface (AGENTS.md §8); this is the *additive* contract
// version.
constexpr auto kAgentVersion = "0.4";
} // namespace

InterfacesService::InterfacesService(NetworkMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    m_clock.start();
    connect(m_monitor, &NetworkMonitor::statsUpdated,
            this,      &InterfacesService::onStatsUpdated);
}

void InterfacesService::setIdleManager(IdleManager *idle)
{
    m_idle = idle;
    if (m_idle) {
        connect(m_idle, &IdleManager::cadenceChanged,
                this,   &InterfacesService::onCadenceChanged);
    }
}

void InterfacesService::setProcessResolver(backend::ProcessResolver *resolver)
{
    m_resolver = resolver;
}

QString InterfacesService::version() const
{
    return QString::fromLatin1(kAgentVersion);
}

QStringList InterfacesService::capabilities() const
{
    // Token grammar: lowercase, dash-separated. Add tokens here when a new
    // optional behaviour lands; never remove or rename. Clients gate
    // behaviour on token presence (defaulting to "off" when absent so
    // older agents still work).
    QStringList base = {
        QStringLiteral("cadence-hints"),     // SetDesiredIntervalMs supported
        QStringLiteral("cadence-signal"),    // CadenceChanged signal emitted
        QStringLiteral("name-owner-cleanup"),// hints dropped on peer disconnect
        QStringLiteral("monotonic-clock"),   // hint expiry uses CLOCK_MONOTONIC
        QStringLiteral("snapshot-cap"),      // ConnectionsChanged capped (top-N)
        QStringLiteral("iana-proto"),        // ConnectionDto.proto is IANA RFC 5237, not L4Proto enum
        QStringLiteral("direction-on-wire"), // ConnectionDto carries server-computed direction
        QStringLiteral("snapshot-timestamp"),// {Stats,Connections}Changed prefix payload with CLOCK_MONOTONIC ms
        QStringLiteral("ifindex"),           // InterfaceStatsDto/ConnectionDto carry kernel ifindex
        QStringLiteral("oper-state"),        // InterfaceStatsDto carries IF_OPER_* per RFC 2863
        QStringLiteral("link-errors"),       // InterfaceStatsDto carries rx/tx errors + drops
        QStringLiteral("tcp-state"),         // ConnectionDto carries conntrack TCP state (TcpState enum)
    };
    if (m_resolver) {
        // Merge resolver tokens (process-attribution, container-attribution,
        // netns-scan, ...) — append-only, never reordered, dedup against the
        // base list as a safety net.
        const auto resolverCaps = m_resolver->capabilities();
        for (const auto &tok : resolverCaps) {
            if (!base.contains(tok)) base.append(tok);
        }
        // v0.4 wire-level mirror of the resolver attribution capabilities:
        // clients gate the new ConnectionDto fields (pid/uid/comm + container
        // bulk + chain) on these tokens rather than poking at fields blindly.
        // Only advertise when the underlying resolver token is present, so
        // we never lie about data we don't actually populate.
        if (resolverCaps.contains(QStringLiteral("process-attribution"))
            && !base.contains(QStringLiteral("process-attribution-wire"))) {
            base.append(QStringLiteral("process-attribution-wire"));
        }
        if (resolverCaps.contains(QStringLiteral("container-attribution"))
            && !base.contains(QStringLiteral("container-attribution-wire"))) {
            base.append(QStringLiteral("container-attribution-wire"));
            // Chain is a strict superset of leaf container info; agents that
            // can fill `container` always fill `containerChain` too (single-
            // entry chain when no nesting is detected).
            base.append(QStringLiteral("container-chain-wire"));
        }
    }
    return base;
}

dbus::InterfaceStatsDtoList InterfacesService::GetInterfaces()
{
    if (m_idle) m_idle->noteActivity();
    return m_last;
}

void InterfacesService::SetDesiredIntervalMs(uint intervalMs)
{
    if (!m_idle) return;
    const QString sender = calledFromDBus() ? message().service() : QString();
    // See ConnectionsService::SetDesiredIntervalMs — only count accepted
    // hints as activity, so a peer rejected from the hint table can't
    // pin the agent at the active cadence indefinitely.
    if (m_idle->setClientHint(sender, static_cast<int>(intervalMs)))
        m_idle->noteActivity();
}

void InterfacesService::onStatsUpdated(const QList<InterfaceStats> &stats)
{
    m_last = dbus::toDtos(stats);
    emit StatsChanged(static_cast<qulonglong>(m_clock.elapsed()), m_last);
}

void InterfacesService::onCadenceChanged(int ms)
{
    emit CadenceChanged(static_cast<uint>(qMax(0, ms)));
}

} // namespace qiftop::agent
