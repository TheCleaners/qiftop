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

    // Transport-neutral capabilities for the DBus path. The agent publishes
    // ONE merged Capabilities list on the Interfaces service, so the network
    // proxy carries the whole thing and the connection proxy returns empty
    // (the client's union recombines them). main.cpp seeds this right after
    // probeAgent() — which already fetched the property — instead of having
    // the proxy re-fetch it.
    void setAgentCapabilities(QStringList caps) { m_caps = std::move(caps); }
    [[nodiscard]] QStringList capabilities() const override { return m_caps; }

signals:
    // Mirrors the agent's CadenceChanged DBus signal: emitted whenever the
    // agent's effective poll interval changes (sped up, slowed down, or
    // paused with ms==0). Lets the UI surface a "agent went quiet" cue
    // without inferring from missing statsUpdated ticks. Always emitted
    // from the main thread.
    void agentCadenceChanged(int intervalMs);

private slots:
    void onStatsChanged(const QDBusMessage &msg);
    void onAgentCadenceChanged(const QDBusMessage &msg);

private:
    void requestInitialSnapshot();
    void sendDesiredIntervalAsync(int ms);

    bool m_useSessionBus = false;
    bool m_started       = false;
    int  m_desiredMs     = 0; // last hint we sent (also acts as heartbeat value)
    QStringList m_caps;       // agent's merged Capabilities list (seeded by main)
};

} // namespace qiftop::backend::dbus_client
