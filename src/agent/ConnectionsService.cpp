#include "ConnectionsService.h"

#include <QDBusConnection>
#include <QDBusMessage>

#include "IdleManager.h"
#include "backend/ConnectionMonitor.h"

namespace qiftop::agent {

ConnectionsService::ConnectionsService(ConnectionMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
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
    m_idle->noteActivity();
    m_idle->setClientHint(sender, static_cast<int>(intervalMs));
}

void ConnectionsService::onConnectionsUpdated(const QList<Connection> &conns)
{
    m_last = dbus::toDtos(conns);
    emit ConnectionsChanged(m_last);
}

void ConnectionsService::onPermissionDenied(const QString &detail)
{
    emit PermissionDenied(detail);
}

void ConnectionsService::onAccountingUnavailable(const QString &detail)
{
    if (m_accountingEnabled) {
        m_accountingEnabled = false;
        emit accountingChanged(false);
    }
    // detail is informational only; PermissionDenied is the actionable signal.
    Q_UNUSED(detail);
}

} // namespace qiftop::agent
