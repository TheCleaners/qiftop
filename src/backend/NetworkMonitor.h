#pragma once

#include <QList>
#include <QObject>
#include <QString>

struct InterfaceStats {
    QString     name;
    QString     type;        // e.g. "ethernet", "bridge", "tun", "loopback", "veth", "vlan"
    quint32     mtu = 0;
    QStringList addresses;   // CIDR strings, link-local & host-scope excluded

    quint64 rxBytes   = 0;
    quint64 txBytes   = 0;
    quint64 rxPackets = 0;
    quint64 txPackets = 0;
    bool    isUp       = false;
    bool    isLoopback = false;
};

Q_DECLARE_METATYPE(InterfaceStats)
Q_DECLARE_METATYPE(QList<InterfaceStats>)

// Abstract base for all platform backends.
// Implementations live in backend/<platform>/ and run their
// polling loop in a worker thread, emitting statsUpdated via
// a queued connection back to the main thread.
class NetworkMonitor : public QObject {
    Q_OBJECT

public:
    explicit NetworkMonitor(QObject *parent = nullptr) : QObject(parent) {}
    ~NetworkMonitor() override = default;

    virtual void start() = 0;
    virtual void stop()  = 0;

    // Dynamic poll-interval control. <= 0 means "pause" (stop emitting until
    // a positive interval is restored). Default is a no-op so subclasses that
    // don't have an internal timer (e.g. the DBus client proxy) can ignore it.
    virtual void setPollIntervalMs(int /*ms*/) {}

    // Tell the producer (potentially a remote agent) what cadence we'd like
    // updates at. For local backends this is usually equivalent to
    // setPollIntervalMs(); for DBus-backed proxies this becomes a remote
    // hint and also functions as an activity heartbeat so the agent's idle
    // manager doesn't decide we've gone away.
    virtual void setDesiredIntervalMs(int /*ms*/) {}

signals:
    // Emitted (from worker thread, queued) at each polling interval.
    void statsUpdated(QList<InterfaceStats> stats);
};
