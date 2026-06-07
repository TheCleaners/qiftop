#pragma once

#include "backend/NetworkMonitor.h"

#include <QDBusMessage>

namespace qiftop::backend::dbus_client {

// Client-side NetworkMonitor implementation that consumes the
// org.qiftop.NetworkAgent1.Interfaces DBus interface published by the
// qiftop-agent daemon.
class DBusNetworkMonitor : public NetworkMonitor {
    Q_OBJECT

public:
    explicit DBusNetworkMonitor(bool useSessionBus = false, QObject *parent = nullptr);
    ~DBusNetworkMonitor() override;

    void start() override;
    void stop()  override;
    void setDesiredIntervalMs(int ms) override;

private slots:
    void onStatsChanged(const QDBusMessage &msg);

private:
    void requestInitialSnapshot();
    void sendDesiredIntervalAsync(int ms);

    bool m_useSessionBus = false;
    bool m_started       = false;
    int  m_desiredMs     = 0; // last hint we sent (also acts as heartbeat value)
};

} // namespace qiftop::backend::dbus_client
