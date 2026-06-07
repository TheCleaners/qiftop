#pragma once

#include <QDBusContext>
#include <QObject>
#include "dbus/Types.h"

class ConnectionMonitor;

namespace qiftop::agent {

class IdleManager;

// Implements the org.qiftop.NetworkAgent1.Connections DBus interface on the
// system bus. Wraps the platform ConnectionMonitor (currently
// ConntrackMonitor on Linux) and rebroadcasts each poll snapshot as
// ConnectionsChanged.
class ConnectionsService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiftop.NetworkAgent1.Connections")
    Q_PROPERTY(bool AccountingEnabled READ accountingEnabled NOTIFY accountingChanged)

public:
    explicit ConnectionsService(ConnectionMonitor *monitor, QObject *parent = nullptr);

    [[nodiscard]] bool accountingEnabled() const { return m_accountingEnabled; }

    void setIdleManager(IdleManager *idle) { m_idle = idle; }

public slots:
    dbus::ConnectionDtoList GetConnections();

    // See InterfacesService::SetDesiredIntervalMs for semantics.
    void SetDesiredIntervalMs(uint intervalMs);

signals:
    Q_SCRIPTABLE void ConnectionsChanged(qiftop::dbus::ConnectionDtoList conns);
    Q_SCRIPTABLE void PermissionDenied(QString detail);
    Q_SCRIPTABLE void accountingChanged(bool enabled);

private slots:
    void onConnectionsUpdated(const QList<Connection> &conns);
    void onPermissionDenied(const QString &detail);
    void onAccountingUnavailable(const QString &detail);

private:
    ConnectionMonitor       *m_monitor = nullptr;
    IdleManager             *m_idle    = nullptr;
    dbus::ConnectionDtoList  m_last;
    bool                     m_accountingEnabled = true;
};

} // namespace qiftop::agent
