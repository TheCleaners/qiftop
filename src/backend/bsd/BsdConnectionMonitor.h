#pragma once

#include <QTimer>

#include "backend/ConnectionMonitor.h"

namespace qiftop::backend::bsd {

// Placeholder per-flow monitor for BSD platforms. NetBSD has no
// conntrack-equivalent kernel flow table; real per-flow byte/packet
// accounting would come from pf state (pfctl -ss / libpfctl) or a
// BPF + userspace accounting datapath (see docs/PORTABILITY.md §2.2).
//
// Until that backend exists this monitor emits no flows and reports
// accounting as unavailable exactly once, so the Connections view
// degrades gracefully instead of looking broken. The interface view
// (BsdNetworkMonitor) is fully functional in the meantime.
class BsdConnectionMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit BsdConnectionMonitor(QObject *parent = nullptr);
    ~BsdConnectionMonitor() override;

    void start() override;
    void stop()  override;

private:
    bool m_warned = false;
};

} // namespace qiftop::backend::bsd
