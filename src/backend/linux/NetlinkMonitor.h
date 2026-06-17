#pragma once

#include <QThread>

#include "backend/NetworkMonitor.h"

class NetlinkWorker;

// Linux NetworkMonitor implementation.
// Owns a QThread and a NetlinkWorker; the worker runs in the
// thread and forwards its statsUpdated signal via a queued
// connection to this object's statsUpdated signal.
class NetlinkMonitor : public NetworkMonitor {
    Q_OBJECT

public:
    explicit NetlinkMonitor(QObject *parent = nullptr);
    ~NetlinkMonitor() override;

    void start() override;
    void stop()  override;
    void setPollIntervalMs(int ms) override;

    // In-process interface stats genuinely fill ifIndex, operState, and the
    // rx/tx error+drop counters (see NetlinkWorker.cpp), so we advertise
    // those structural tokens — making the GUI/TUI treat the in-process path
    // as a first-class capability source, not "agent only".
    [[nodiscard]] QStringList capabilities() const override;

private:
    QThread       m_thread;
    NetlinkWorker *m_worker = nullptr; // owned by m_thread
};
