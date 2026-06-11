#pragma once

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QSet>
#include <QTimer>

#include "backend/Connection.h"

struct pcap;
typedef struct pcap pcap_t;

class QSocketNotifier;

namespace qiftop::backend::bsd {

// Per-interface pcap capture handle plus the metadata needed to parse and
// attribute its packets.
struct CaptureHandle {
    pcap_t         *pcap     = nullptr;
    QSocketNotifier *notifier = nullptr; // readable-fd notifier (owned)
    int             datalink = 0;        // DLT_* link-layer type
    QString         iface;
    quint32         ifIndex  = 0;
};

// Userspace per-flow accumulator. Counters are cumulative since first sight;
// the aggregator turns them into rates by differencing successive snapshots.
struct FlowAcc {
    Endpoint local;   // host side of the flow
    Endpoint remote;  // peer side
    L4Proto  proto = L4Proto::Unknown;
    quint64  rxBytes = 0, txBytes = 0, rxPackets = 0, txPackets = 0;
    QString  iface;
    quint32  ifIndex = 0;
    qint64   lastSeenMs = 0;
    // Direction observed from the TCP handshake (the SYN sender is the
    // initiator). More reliable than the ephemeral-port heuristic, which
    // misfires when peers use different local port ranges. Unknown until a
    // SYN-without-ACK is seen (e.g. UDP, or capture started mid-connection).
    Direction observedDir = Direction::Unknown;
};

// Runs inside the worker thread owned by BsdConnectionMonitor. Sniffs IP
// traffic on every up interface via libpcap/BPF (the portable BSD/macOS
// datapath — there is no conntrack), maintains a 5-tuple flow table, and
// emits a full snapshot each tick. This is the same model iftop uses.
//
// Design note: the packet-parsing + flow-accounting logic here is
// deliberately platform-neutral (libpcap is cross-platform; only the
// getifaddrs/sysctl local-state probe is BSD-flavoured). It is a candidate
// to backport to Linux as an alternative/complementary capture path (a
// pcap data source selectable at build or run time) alongside the conntrack
// backend, so keep the pure logic free of BSD-only assumptions.
//
// Requires read access to /dev/bpf* (root on the BSDs). When capture can't
// be opened on any interface it emits accountingUnavailable once and goes
// quiet, exactly like the previous stub.
class BsdConnectionWorker : public QObject {
    Q_OBJECT

public:
    explicit BsdConnectionWorker(QObject *parent = nullptr);
    ~BsdConnectionWorker() override;

public slots:
    void start();
    void stop();
    void setPollIntervalMs(int ms);

signals:
    void connectionsUpdated(QList<Connection> connections);
    void accountingUnavailable(QString detail);

private slots:
    void onReadable(int fd);
    void emitSnapshot();

private:
    void refreshLocalState();
    void openCaptures();
    void closeCaptures();
    void handlePacket(const CaptureHandle &h, const quint8 *data, quint32 caplen,
                      quint32 wireLen);

    QList<CaptureHandle> m_handles;
    QHash<QString, FlowAcc> m_flows;     // keyed by normalized flow key
    QSet<QHostAddress>   m_localAddrs;
    QSet<QHostAddress>   m_loopbackAddrs;
    quint16              m_ephLow  = 49152;
    quint16              m_ephHigh = 65535;
    QTimer              *m_timer   = nullptr;
    bool                 m_warned  = false;
    bool                 m_flowCapWarned = false;
};

} // namespace qiftop::backend::bsd
