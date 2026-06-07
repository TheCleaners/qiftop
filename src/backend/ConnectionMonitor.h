#pragma once

#include <QObject>

#include "Connection.h"

// Abstract base for per-connection traffic monitors.
//
// Implementations live in backend/<platform>/ and run their capture loop
// in a worker thread, emitting connectionsUpdated via a queued connection.
//
// Each update should contain the full current set of active flows; the model
// is responsible for diffing/expiring.
class ConnectionMonitor : public QObject {
    Q_OBJECT

public:
    explicit ConnectionMonitor(QObject *parent = nullptr);
    ~ConnectionMonitor() override;

    virtual void start() = 0;
    virtual void stop()  = 0;
    virtual void setPollIntervalMs(int /*ms*/) {}
    virtual void setDesiredIntervalMs(int /*ms*/) {}

signals:
    void connectionsUpdated(QList<Connection> connections);
    void permissionDenied(QString detail);

    // Emitted (once) when the backend determines that the kernel is not
    // recording per-flow byte/packet counters (e.g. Linux's
    // `net.netfilter.nf_conntrack_acct` sysctl is off and we couldn't turn
    // it on). The UI surfaces this as a non-fatal hint.
    void accountingUnavailable(QString detail);
};
