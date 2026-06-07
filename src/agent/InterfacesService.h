#pragma once

#include <QDBusContext>
#include <QObject>
#include "dbus/Types.h"

class NetworkMonitor;

namespace qiftop::agent {

class IdleManager;

// Implements the org.qiftop.NetworkAgent1.Interfaces DBus interface on the
// system bus. Wraps the platform NetworkMonitor (currently NetlinkMonitor on
// Linux) and rebroadcasts its periodic snapshots as a `StatsChanged` signal.
class InterfacesService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiftop.NetworkAgent1.Interfaces")

public:
    explicit InterfacesService(NetworkMonitor *monitor, QObject *parent = nullptr);

    void setIdleManager(IdleManager *idle) { m_idle = idle; }

public slots:
    // DBus method: returns the most recent per-interface snapshot. Useful
    // for late-joining clients that don't want to wait for the next tick.
    dbus::InterfaceStatsDtoList GetInterfaces();

    // DBus method: clients request a desired polling cadence in ms.
    // The agent honours min() across all active hints (clamped to the
    // configured floor). Hints expire after Config::hintTtlMs unless
    // re-asserted. Pass 0 (or omit) to clear this client's hint.
    void SetDesiredIntervalMs(uint intervalMs);

signals:
    // DBus signal — fires on each polling tick of the backend.
    Q_SCRIPTABLE void StatsChanged(qiftop::dbus::InterfaceStatsDtoList stats);

private slots:
    void onStatsUpdated(const QList<InterfaceStats> &stats);

private:
    NetworkMonitor              *m_monitor = nullptr;
    IdleManager                 *m_idle    = nullptr;
    dbus::InterfaceStatsDtoList  m_last;
};

} // namespace qiftop::agent
