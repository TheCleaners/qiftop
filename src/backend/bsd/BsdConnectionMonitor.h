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

    // The libpcap/BPF path fills proto (iana-proto) and a real per-flow
    // direction (direction-on-wire — observed from the TCP SYN, or
    // inferDirection() fallback). It attributes flows to a PID via
    // BsdSocketResolver (process-attribution-wire) and, on FreeBSD,
    // tags jailed flows runtime:jail (container-attribution-wire, gated by
    // #ifdef __FreeBSD__). It does NOT advertise tcp-state (pcap has no
    // conntrack state machine), container-chain-wire (no nesting model),
    // or attribution-reason (inferred client-side).
    [[nodiscard]] QStringList capabilities() const override;

private:
    QThread              m_thread;
    BsdConnectionWorker *m_worker = nullptr; // owned by m_thread
};

} // namespace qiftop::backend::bsd
