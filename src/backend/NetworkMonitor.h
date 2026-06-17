#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

struct InterfaceStats {
    QString     name;
    QString     type;        // e.g. "ethernet", "bridge", "tun", "loopback", "veth", "vlan"
    quint32     mtu = 0;
    QStringList addresses;   // CIDR strings, link-local & host-scope excluded

    quint64 rxBytes   = 0;
    quint64 txBytes   = 0;
    quint64 rxPackets = 0;
    quint64 txPackets = 0;
    bool    isUp       = false;     // IFF_UP (admin) — kept for back-compat
    bool    isLoopback = false;

    quint32 ifIndex   = 0;          // Kernel ifindex (0 = unknown).

    // Linux IF_OPER_* per RFC 2863:
    //   0 UNKNOWN, 1 NOTPRESENT, 2 DOWN, 3 LOWERLAYERDOWN,
    //   4 TESTING, 5 DORMANT, 6 UP.
    // 0 when the backend can't determine it.
    quint8  operState = 0;

    // Cumulative kernel counters since interface creation.
    quint64 rxErrors  = 0;
    quint64 txErrors  = 0;
    quint64 rxDropped = 0;
    quint64 txDropped = 0;
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

    // Capability tokens this backend's data path actually provides, in the
    // SAME vocabulary as the agent's DBus `Capabilities` property
    // (AGENTS.md §4). Capabilities are a TRANSPORT-NEUTRAL property of the
    // active backend, not an agent-only concept: the DBus proxy reports the
    // agent's advertised list, while in-process backends report the tokens
    // their own data path delivers. The client gates optional UI on the
    // UNION of the active monitors' capabilities, regardless of transport.
    // Default empty so backends that produce nothing extra are unaffected.
    // Only ever advertise a token for data this backend genuinely fills.
    [[nodiscard]] virtual QStringList capabilities() const { return {}; }

signals:
    // Emitted (from worker thread, queued) at each polling interval.
    void statsUpdated(QList<InterfaceStats> stats);
};
