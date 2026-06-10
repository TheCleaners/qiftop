#pragma once

#include "backend/ConnectionMonitor.h"

#include <QDBusMessage>

namespace qiftop::backend::dbus_client {

class DBusConnectionMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit DBusConnectionMonitor(bool useSessionBus = false, QObject *parent = nullptr);
    ~DBusConnectionMonitor() override;

    void start() override;
    void stop()  override;
    void setDesiredIntervalMs(int ms) override;
    void requestProcessDetails(qint32 pid) override;

private slots:
    void onConnectionsChanged(const QDBusMessage &msg);
    void onPermissionDenied(const QString &detail);

private:
    void requestInitialSnapshot();
    void sendDesiredIntervalAsync(int ms);

    bool m_useSessionBus = false;
    bool m_started       = false;
    int  m_desiredMs     = 0;
};

} // namespace qiftop::backend::dbus_client
