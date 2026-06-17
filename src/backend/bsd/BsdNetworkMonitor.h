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

    // The getifaddrs(3) AF_LINK path fills ifIndex (if_nametoindex),
    // operState (mapped from ifi_link_state), and the rx/tx error+drop
    // counters from `struct if_data` (txDropped stays 0 — BSD has no
    // output-drop counter, but the rest are real). So we advertise the same
    // interface tokens as the Linux NetlinkMonitor — the GUI/TUI light up
    // the same columns on BSD.
    [[nodiscard]] QStringList capabilities() const override;

private:
    QThread          m_thread;
    BsdNetworkWorker *m_worker = nullptr; // owned by m_thread
};

} // namespace qiftop::backend::bsd
