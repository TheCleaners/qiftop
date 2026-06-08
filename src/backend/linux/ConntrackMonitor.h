#pragma once

#include <QThread>

#include "backend/ConnectionMonitor.h"

// Linux ConnectionMonitor — libnetfilter_conntrack implementation.
//
// Owns a QThread; the worker polls conntrack on a tick and emits
// connectionsUpdated with the current flow set + per-flow byte/packet
// counters. Issues separate AF_INET and AF_INET6 dumps because the
// AF_UNSPEC dump path is unreliable across kernel/libnetfilter_conntrack
// combos and often returns only IPv4 entries.
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
