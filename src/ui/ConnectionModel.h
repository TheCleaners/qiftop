#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QSet>

#include <deque>

#include "backend/Connection.h"
#include "util/Exportable.h"

class DnsResolver;

class ConnectionModel : public QAbstractTableModel, public Exportable {
    Q_OBJECT

public:
    enum class Column : int {
        Iface,        // egress interface name (may be empty for unattributed)
        Flow,         // rendered as: [proto] local → remote (via delegate)
        RxRate,
        TxRate,
        RxTotal,
        TxTotal,
        RxMax,        // adaptive reference (window-max or CMA) — see setThroughput*
        TxMax,
        Process,      // comm + pid (v0.2 attribution; hidden by default)
        Container,    // runtime:name + chain breadcrumb (v0.2 attribution; hidden by default)
        ColumnCount,
    };

    enum Role {
        SortRole = Qt::UserRole + 1,
        IsIPv6Role,
        ProtoTextRole,    // QString: "TCP", "UDP", ...
        ProtoRole,        // int: L4Proto value
        LocalTextRole,    // QString: "host:port" (hostname if resolved)
        RemoteTextRole,   // QString: "host:port" (hostname if resolved)
        IsStaleRole,      // bool — flow was absent from the latest backend tick
        IfaceNameRole,    // QString: backend-reported egress ifname (raw)
        DirectionRole,    // int: Connection::Direction value
        GaugeFractionRole,    // double in [0,1] (current combined / max combined)
        GaugeDarkColorRole,   // QColor: "filled" portion of the gauge
        ConnectionRole,       // Connection (full value) — for filter eval
        RxRateRole,           // double bytes/s (smoothed)
        TxRateRole,           // double bytes/s (smoothed)
        HostnameLocalRole,    // QString (resolved local hostname, may be empty)
        HostnameRemoteRole,   // QString (resolved remote hostname, may be empty)
        ProcessPidRole,       // qint32: process.pid (0 if unattributed)
        ProcessCommRole,      // QString: process.comm
        ContainerRuntimeRole, // QString: container.runtime (e.g. "docker")
        ContainerIdRole,      // QString: container.id (12-char shortform safe)
        ContainerNameRole,    // QString: container.name (display name)
        ContainerChainRole,   // QStringList: "runtime:name" per ancestry layer
    };

    // Adaptive throughput tracking modes (mirror Settings::ThroughputMaxMode).
    enum class ThroughputMaxMode : int { Windowed = 0, CumulativeAverage = 1 };

    explicit ConnectionModel(QObject *parent = nullptr);

    // Optional DNS resolver. When set AND enabled, address columns display
    // hostnames as they become available. Pass nullptr to display raw IPs.
    void setDnsResolver(DnsResolver *resolver);
    void setHostnameResolutionEnabled(bool enabled);

    // Set of "this host"'s addresses (any interface, any family). The
    // model strips loopback addresses (127.0.0.1 / ::1) from this set on
    // assignment and tracks them separately so they can always be
    // rendered as "localhost" regardless of the iface-as-localhost
    // toggle. When DNS resolution is enabled and the per-toggle is on,
    // iface addresses are also rendered as "localhost".
    void setLocalAddresses(QSet<QHostAddress> addrs);

    // Retention windows for closed flows. UDP gets its own knob because
    // UDP flows are intrinsically bursty (one-shot DNS, etc.) and the
    // kernel times conntrack entries out aggressively.
    void setStaleRetentionMs(int ms)    { m_staleRetentionMs    = ms < 0 ? 0 : ms; }
    void setStaleRetentionMsUdp(int ms) { m_staleRetentionMsUdp = ms < 0 ? 0 : ms; }

    // If true, UDP flows sharing a peer are coalesced into a single row;
    // the ephemeral side is displayed as "*". Applied on each tick before
    // the model diff, so toggling it only takes effect on the next update.
    void setUdpAggregateByPeer(bool v)  { m_udpAggregateByPeer = v; }

    // When false, the Container column's tooltip stops listing the
    // OUTER→INNER nesting breakdown (one-line summary only). Gated in
    // the UI by the agent's container-chain-wire capability; with no
    // chain data the breakdown wouldn't render anyway.
    void setShowContainerChainInTooltip(bool v) { m_showContainerChainInTooltip = v; }

    // When true, the entire row gets a faint background tint based on
    // its inferred direction (green = outbound, red = inbound). Unknown
    // direction = no tint. Repaints all rows on change.
    void setTintRowByDirection(bool v);

    // If true (and DNS resolution is on), this host's own iface addresses
    // are rendered as "localhost". Loopback (127.0.0.1/::1) is always
    // shown as "localhost".
    void setResolveIfaceAddrsAsLocalhost(bool v);

    // --- adaptive throughput gauge ---
    // When enabled, the model tracks a per-row "max" reference value for
    // rx and tx rates (over either a sliding window or a cumulative
    // average) and exposes a 0..1 gauge fraction via GaugeFractionRole.
    // The two RxMax/TxMax columns also start populating with this value.
    void setThroughputGaugeEnabled(bool v);
    void setThroughputMaxMode(ThroughputMaxMode m);
    // Sliding window length, in milliseconds. Only meaningful in
    // Windowed mode. Shorter windows make the gauge more reactive;
    // longer windows make it a more stable baseline.
    void setThroughputWindowMs(int ms);
    // EMA time constant for the smoothing pipeline's target stage, in
    // milliseconds. 0 = no smoothing (display = raw). The downstream
    // gauge / Max columns / sliding-window samples / CMA all read raw
    // rates; only the per-row display value is smoothed (and then
    // tweened in advanceSmoothing()).
    void setRateSmoothingMs(int ms) { m_rateSmoothingMs = ms < 0 ? 0 : ms; }
    // Baseline animation duration for the display tween (typically the
    // backend poll interval). Falls use ~1/3 of this for a snappier
    // "rates fall faster" feel. Has no effect when smoothing is off.
    void setPollIntervalMs(int ms)  { m_pollIntervalMs  = ms < 0 ? 0 : ms; }

    [[nodiscard]] int      rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] int      columnCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    // Exportable
    [[nodiscard]] QStringList  exportHeaders() const override;
    [[nodiscard]] int          exportRowCount() const override;
    [[nodiscard]] QVariantList exportRow(int row) const override;

    // ----- helpers for the context-menu copy actions -----
    // Returns true and fills *out on success. row is a *source* model row.
    // "Copy semantics" differ from display semantics in one way: an iface
    // address that the Flow column shows as "localhost" is copied as its
    // numeric IP, because "localhost" is meaningless outside this host.
    // Loopback (127.0.0.1, ::1) is still copied as "localhost".
    enum class FlowEnd { Source, Destination };
    [[nodiscard]] QString copyTextForEndpoint(int row, FlowEnd which) const;
    [[nodiscard]] QString copyTextForFlow(int row) const;
    // Returns the numeric address of the remote (peer) endpoint of `row`
    // as plain text (no port, no hostname). Used to seed a filter
    // expression like `host="<addr>"` from the connection context menu.
    // Empty on invalid row or null address.
    [[nodiscard]] QString peerAddressText(int row) const;

public slots:
    void updateConnections(QList<Connection> connections);
    // Sub-poll display animation: advances each row's smoothed display
    // rate (EMA) toward its last raw value at the elapsed dt since the
    // previous smoothing step. No-op if smoothing is off. Designed to
    // be driven by a QTimer in MainWindow at a higher cadence than the
    // backend poll, so rate changes ease in/out visually between polls.
    void advanceSmoothing();

private slots:
    void onResolved(QHostAddress addr, QString hostname);

private:
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
        // Adaptive throughput tracking — kept up-to-date regardless of
        // the active mode, so toggling Windowed<->CMA doesn't require
        // resetting stats. Samples are RAW rates (not smoothed) so the
        // reference (Max columns / gauge denominator) reflects honest
        // throughput rather than inheriting the display smoothing lag.
        std::deque<RateSample> rxSamples;
        std::deque<RateSample> txSamples;
        double  rxSum   = 0.0;
        double  txSum   = 0.0;
        quint64 samples = 0;
    };

    [[nodiscard]] QString displayHost(const QHostAddress &addr) const;
    [[nodiscard]] QString endpointText(const Endpoint &ep) const;
    [[nodiscard]] QString endpointCopyText(const Endpoint &ep) const;
    [[nodiscard]] QString protoLabel(const Connection &c) const;
    void requestResolution(const QHostAddress &addr);
    // True iff neither end of the flow is one of this host's own addresses
    // (loopback or interface). Such flows are being routed *through* this
    // host (NAT/masquerade, IP forwarding) and the inbound/outbound
    // direction relative to "us" is meaningless — we render them with a
    // distinct tint so they don't get lumped in with "couldn't infer".
    [[nodiscard]] bool isForwardedFlow(const Connection &c) const;
    // Returns the per-direction adaptive reference values for the row.
    [[nodiscard]] double rxReference(const Row &r) const;
    [[nodiscard]] double txReference(const Row &r) const;

    QList<Row>                m_rows;       // sorted by Connection::key
    QHash<QString, qsizetype> m_keyToIdx;
    QHash<QString, Connection> m_prev;
    QElapsedTimer             m_elapsed;
    qint64                    m_lastElapsedMs = 0;

    DnsResolver *m_resolver       = nullptr;
    bool         m_resolveEnabled = false;
    int          m_staleRetentionMs    = 15'000;
    int          m_staleRetentionMsUdp = 60'000;
    bool         m_udpAggregateByPeer  = true;
    bool         m_tintRowByDirection  = false;
    bool         m_aliasIfaceAddrsAsLocalhost = true;
    bool         m_showContainerChainInTooltip = true;
    bool         m_gaugeEnabled        = false;
    ThroughputMaxMode m_maxMode        = ThroughputMaxMode::Windowed;
    int          m_windowMs            = 30'000;
    int          m_rateSmoothingMs     = 0;     // EMA τ in ms; 0 = off
    int          m_pollIntervalMs      = 1000;  // baseline tween duration
    // /proc/sys/net/ipv4/ip_local_port_range (cached at construction);
    // used to heuristically tag flow Direction.
    quint16      m_ephemeralLow  = 32768;
    quint16      m_ephemeralHigh = 60999;
    QSet<QHostAddress> m_loopbackAddrs;
    QSet<QHostAddress> m_localAddrs;
};
