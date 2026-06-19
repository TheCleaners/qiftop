#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSet>

#include <deque>

#include "backend/Connection.h"

class DnsResolver;

namespace qiftop::aggregate {

// Plain-QObject aggregator for the per-connection flow table: the 3-layer
// raw -> target(EMA) -> display(tween) rate-smoothing pipeline, UDP
// peer aggregation, DNS hostname resolution, direction inference, the
// adaptive throughput reference (windowed-max / cumulative-average), and
// stale-flow retention. Extracted from ConnectionModel so EVERY frontend
// (the Qt GUI's ConnectionModel, the ncurses nqiftop, a future exporter)
// shares one implementation with NO QAbstractItemModel / Widgets / QtGui
// dependency. It lives in libqiftop and emits GRANULAR change signals that
// a model adapter maps 1:1 onto begin/endInsertRows / begin/endRemoveRows /
// dataChanged, preserving exact view semantics.
//
// The Qt-Gui-specific rendering (colours, fonts, gauge painting) stays in
// the model/delegates and reads this aggregator's data + helpers.
class ConnectionAggregator : public QObject {
    Q_OBJECT

public:
    enum class ThroughputMaxMode : int { Windowed = 0, CumulativeAverage = 1 };
    enum class FlowEnd { Source, Destination };

    struct RateSample {
        qint64 timeMs;
        double rate;
    };

    struct Row {
        Connection current{};
        double  rxRate     = 0.0;   // smoothed (display, tweened)
        double  txRate     = 0.0;
        double  rxRaw      = 0.0;   // last raw rate from a real poll
        double  txRaw      = 0.0;
        double  rxTarget   = 0.0;   // EMA-smoothed target (animation endpoint)
        double  txTarget   = 0.0;
        double  rxAnimFrom = 0.0;   // tween start values, captured at each poll
        double  txAnimFrom = 0.0;
        qint64  rxAnimStartMs = 0;
        qint64  txAnimStartMs = 0;
        int     rxAnimDurMs   = 0;  // 0 = no animation in progress
        int     txAnimDurMs   = 0;
        qint64  lastSeenMs = 0;
        bool    stale      = false;
        std::deque<RateSample> rxSamples;
        std::deque<RateSample> txSamples;
        double  rxSum   = 0.0;
        double  txSum   = 0.0;
        quint64 samples = 0;
    };

    explicit ConnectionAggregator(QObject *parent = nullptr);

    // --- computation / DNS settings (forwarded from the model setters) ---
    void setDnsResolver(DnsResolver *resolver);
    void setHostnameResolutionEnabled(bool enabled);
    void setLocalAddresses(QSet<QHostAddress> addrs);
    void setStaleRetentionMs(int ms)    { m_staleRetentionMs    = ms < 0 ? 0 : ms; }
    void setStaleRetentionMsUdp(int ms) { m_staleRetentionMsUdp = ms < 0 ? 0 : ms; }
    void setUdpAggregateByPeer(bool v)
    {
        if (v != m_udpAggregateByPeer) m_udpAgg.clear();
        m_udpAggregateByPeer = v;
    }
    void setResolveIfaceAddrsAsLocalhost(bool v);
    void setThroughputMaxMode(ThroughputMaxMode m);
    void setThroughputWindowMs(int ms);
    void setRateSmoothingMs(int ms) { m_rateSmoothingMs = ms < 0 ? 0 : ms; }
    void setPollIntervalMs(int ms)  { m_pollIntervalMs  = ms < 0 ? 0 : ms; }

    // --- query ---
    [[nodiscard]] int               rowCount() const { return static_cast<int>(m_rows.size()); }
    [[nodiscard]] const QList<Row> &rows() const { return m_rows; }
    [[nodiscard]] const Row        &rowAt(int i) const { return m_rows[i]; }
    [[nodiscard]] bool              resolveEnabled() const { return m_resolveEnabled; }

    // --- pure-ish derivation helpers (used by the model's data() and the TUI) ---
    [[nodiscard]] double  rxReference(const Row &r) const;
    [[nodiscard]] double  txReference(const Row &r) const;
    [[nodiscard]] bool    isForwardedFlow(const Connection &c) const;
    [[nodiscard]] QString displayHost(const QHostAddress &addr) const;
    [[nodiscard]] QString endpointText(const Endpoint &ep) const;
    [[nodiscard]] QString protoLabel(const Connection &c) const;
    // Resolver cache lookup gated on resolution-enabled (empty otherwise).
    [[nodiscard]] QString cachedHostname(const QHostAddress &addr) const;
    // Context-menu copy helpers (row is an aggregator row index).
    [[nodiscard]] QString copyTextForEndpoint(int row, FlowEnd which) const;
    [[nodiscard]] QString copyTextForFlow(int row) const;
    [[nodiscard]] QString peerAddressText(int row) const;

public slots:
    void updateConnections(QList<Connection> connections);
    void advanceSmoothing();

    // Apply an attribution-only patch (v0.4 §5 deep pass). For each entry,
    // find the live row by Connection::key and update ONLY its process /
    // container / chain / reason fields — never bytes, rates, or timestamps,
    // so a refinement arriving between polls can't perturb the rate series.
    // Non-matching keys (flow gone, or a UDP-aggregated row) are skipped.
    void applyAttributionPatch(const QList<Connection> &patch);

signals:
    // Granular signals a model adapter maps 1:1 onto the QAbstractItemModel
    // protocol. The aggregator emits *AboutTo* BEFORE mutating m_rows and the
    // completion signal AFTER, so a synchronous adapter brackets the mutation
    // exactly like begin/endInsertRows + begin/endRemoveRows expect.
    void rowsAboutToBeInserted(int first, int last);
    void rowsInserted();
    void rowsAboutToBeRemoved(int first, int last);
    void rowsRemoved();
    void rowsUpdated(int first, int last);
    // Coarse "repaint everything" for a settings change or a DNS resolution
    // (the rendered text depends on resolver state, not on row identity).
    void viewDataChanged();

private slots:
    void onResolved(const QHostAddress &addr, const QString &hostname);

private:
    [[nodiscard]] QString endpointCopyText(const Endpoint &ep) const;
    void requestResolution(const QHostAddress &addr);

    QList<Row>                 m_rows;       // sorted by Connection::key
    QHash<QString, qsizetype>  m_keyToIdx;
    QHash<QString, Connection> m_prev;
    QElapsedTimer              m_elapsed;
    qint64                     m_lastElapsedMs = 0;

    struct UdpAggMember {
        quint64 rxBytes = 0, txBytes = 0;
        quint64 rxPackets = 0, txPackets = 0;
        qint64  lastSeenMs = 0;
    };
    struct UdpAggState {
        quint64 rxBase = 0, txBase = 0;
        quint64 rxPktBase = 0, txPktBase = 0;
        QHash<QString, UdpAggMember> members;
        qint64 lastSeenMs = 0;
    };
    QHash<QString, UdpAggState> m_udpAgg;

    DnsResolver *m_resolver       = nullptr;
    bool         m_resolveEnabled = false;
    int          m_staleRetentionMs    = 15'000;
    int          m_staleRetentionMsUdp = 60'000;
    bool         m_udpAggregateByPeer  = true;
    bool         m_aliasIfaceAddrsAsLocalhost = true;
    ThroughputMaxMode m_maxMode        = ThroughputMaxMode::Windowed;
    int          m_windowMs            = 30'000;
    int          m_rateSmoothingMs     = 0;
    int          m_pollIntervalMs      = 1000;
    quint16      m_ephemeralLow  = 32768;
    quint16      m_ephemeralHigh = 60999;
    QSet<QHostAddress> m_loopbackAddrs;
    QSet<QHostAddress> m_localAddrs;
};

} // namespace qiftop::aggregate
