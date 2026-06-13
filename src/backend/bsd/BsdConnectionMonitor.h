#pragma once

#include <QThread>

#include "backend/ConnectionMonitor.h"

namespace qiftop::backend::bsd {

class BsdConnectionWorker;

// BSD per-flow monitor. Owns a QThread running a BsdConnectionWorker that
// sniffs IP traffic via libpcap/BPF and maintains a userspace flow table
// (NetBSD/FreeBSD/OpenBSD/DragonFly/macOS — there is no conntrack). When
// capture can't start (no /dev/bpf access) it reports accounting
// unavailable and stays quiet, so the Connections view degrades gracefully.
class BsdConnectionMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit BsdConnectionMonitor(QObject *parent = nullptr);
    ~BsdConnectionMonitor() override;

    void start() override;
    void stop()  override;
    void setPollIntervalMs(int ms) override;

private:
    QThread              m_thread;
    BsdConnectionWorker *m_worker = nullptr; // owned by m_thread
};

} // namespace qiftop::backend::bsd
