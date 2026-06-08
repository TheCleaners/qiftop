#include "InterfacesService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include "IdleManager.h"
#include "backend/NetworkMonitor.h"

namespace qiftop::agent {

namespace {
// Bump alongside CMake `project(... VERSION ...)`. Read by clients via
// the standard Properties.Get to display "Connected to qiftop-agent vX"
// and gate optional feature use. Breaking wire changes still require a
// NetworkAgent2 interface (AGENTS.md §8); this is the *additive* contract
// version.
constexpr auto kAgentVersion = "0.2";
} // namespace

InterfacesService::InterfacesService(NetworkMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
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
    return {
        QStringLiteral("cadence-hints"),     // SetDesiredIntervalMs supported
        QStringLiteral("cadence-signal"),    // CadenceChanged signal emitted
        QStringLiteral("name-owner-cleanup"),// hints dropped on peer disconnect
        QStringLiteral("monotonic-clock"),   // hint expiry uses CLOCK_MONOTONIC
        QStringLiteral("snapshot-cap"),      // ConnectionsChanged capped (top-N)
        QStringLiteral("iana-proto"),        // ConnectionDto.proto is IANA RFC 5237, not L4Proto enum
        QStringLiteral("direction-on-wire"), // ConnectionDto carries server-computed direction
    };
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
    m_idle->noteActivity();
    m_idle->setClientHint(sender, static_cast<int>(intervalMs));
}

void InterfacesService::onStatsUpdated(const QList<InterfaceStats> &stats)
{
    m_last = dbus::toDtos(stats);
    emit StatsChanged(m_last);
}

void InterfacesService::onCadenceChanged(int ms)
{
    emit CadenceChanged(static_cast<uint>(qMax(0, ms)));
}

} // namespace qiftop::agent
