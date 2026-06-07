#pragma once

#include <QThread>

#include "backend/ConnectionMonitor.h"

// Linux ConnectionMonitor — placeholder scaffold.
//
// Owns a QThread and emits connectionsUpdated on a periodic tick.
// The actual capture path is intentionally stubbed: future work plugs in
// either libnetfilter_conntrack (preferred for flow-level stats with no
// per-packet cost) or libpcap (for true wire-level counting like iftop).
class ConntrackMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit ConntrackMonitor(QObject *parent = nullptr);
    ~ConntrackMonitor() override;

    void start() override;
    void stop()  override;
    void setPollIntervalMs(int ms) override;

private:
    class Worker;

    QThread  m_thread;
    Worker  *m_worker = nullptr; // owned by m_thread
};
