#pragma once

#include <QThread>

#include "backend/NetworkMonitor.h"

namespace qiftop::backend::bsd {

class BsdNetworkWorker;

// NetBSD/BSD NetworkMonitor implementation. Owns a QThread and a
// BsdNetworkWorker that polls getifaddrs(3) for per-interface counters,
// forwarding statsUpdated back to the main thread via a queued connection.
class BsdNetworkMonitor : public NetworkMonitor {
    Q_OBJECT

public:
    explicit BsdNetworkMonitor(QObject *parent = nullptr);
    ~BsdNetworkMonitor() override;

    void start() override;
    void stop()  override;
    void setPollIntervalMs(int ms) override;

private:
    QThread          m_thread;
    BsdNetworkWorker *m_worker = nullptr; // owned by m_thread
};

} // namespace qiftop::backend::bsd
