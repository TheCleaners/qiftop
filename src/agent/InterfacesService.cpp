#include "InterfacesService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include "IdleManager.h"
#include "backend/NetworkMonitor.h"

namespace qiftop::agent {

InterfacesService::InterfacesService(NetworkMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    connect(m_monitor, &NetworkMonitor::statsUpdated,
            this,      &InterfacesService::onStatsUpdated);
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

} // namespace qiftop::agent
