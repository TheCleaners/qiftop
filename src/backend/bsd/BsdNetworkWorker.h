#pragma once

#include <QObject>
#include <QTimer>

#include "backend/NetworkMonitor.h"

namespace qiftop::backend::bsd {

// Runs inside the worker thread owned by BsdNetworkMonitor.
// Polls per-interface counters via getifaddrs(3) AF_LINK records
// (portable across the BSDs and macOS) and emits statsUpdated on
// each tick. No external library dependency.
class BsdNetworkWorker : public QObject {
    Q_OBJECT

public:
    explicit BsdNetworkWorker(QObject *parent = nullptr);
    ~BsdNetworkWorker() override;

public slots:
    void start();
    void stop();
    void setPollIntervalMs(int ms);

signals:
    void statsUpdated(QList<InterfaceStats> stats);

private slots:
    void poll();

private:
    QTimer *m_timer = nullptr;
};

} // namespace qiftop::backend::bsd
