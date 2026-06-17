#pragma once

#include <QAbstractTableModel>
#include <QHostAddress>
#include <QSet>

#include "aggregate/ConnectionAggregator.h"
#include "backend/Connection.h"
#include "util/Exportable.h"

class DnsResolver;

// Thin QAbstractTableModel adapter over qiftop::aggregate::ConnectionAggregator
// (libqiftop). The aggregator owns ALL the data + the smoothing / UDP-agg /
// DNS / direction / throughput-reference logic; this class only translates
// the aggregator's granular change signals into the QAbstractItemModel
// protocol and renders the Qt-Gui-specific bits (colours, fonts, gauge,
// tooltips) in data(). Its public interface is unchanged from the pre-split
// model, so MainWindow / main.cpp / the smoke tests are untouched.
class ConnectionModel : public QAbstractTableModel, public Exportable {
    Q_OBJECT

public:
    enum class Column : int {
        Iface,        // egress interface name (may be empty for unattributed)
        Flow,         // rendered as: [proto] local -> remote (via delegate)
        RxRate,
        TxRate,
        RxTotal,
        TxTotal,
        RxMax,        // adaptive reference (window-max or CMA) — see setThroughput*
        TxMax,
        Process,      // comm + pid (v0.2 attribution; shown when agent capable)
        Container,    // runtime:name + chain breadcrumb (v0.2 attribution; shown when agent capable)
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
        GroupChipsRole,       // QVariantList of {"text":QString,"kind":QString}
                              // — colour-codable group-header segments
                              // (ConnectionGroupProxy group rows only;
                              // empty for flow rows).
    };

    // Adaptive throughput tracking modes (mirror Settings::ThroughputMaxMode).
    enum class ThroughputMaxMode : int { Windowed = 0, CumulativeAverage = 1 };

    explicit ConnectionModel(QObject *parent = nullptr);

    // Optional DNS resolver. When set AND enabled, address columns display
    // hostnames as they become available. Pass nullptr to display raw IPs.
    void setDnsResolver(DnsResolver *resolver) { m_agg.setDnsResolver(resolver); }
    void setHostnameResolutionEnabled(bool enabled) { m_agg.setHostnameResolutionEnabled(enabled); }

    // Set of "this host"'s addresses (any interface, any family).
    void setLocalAddresses(QSet<QHostAddress> addrs) { m_agg.setLocalAddresses(std::move(addrs)); }

    // Retention windows for closed flows (UDP gets its own knob).
    void setStaleRetentionMs(int ms)    { m_agg.setStaleRetentionMs(ms); }
    void setStaleRetentionMsUdp(int ms) { m_agg.setStaleRetentionMsUdp(ms); }

    // If true, UDP flows sharing a peer are coalesced into a single row.
    void setUdpAggregateByPeer(bool v) { m_agg.setUdpAggregateByPeer(v); }

    // When false, the Container column's tooltip stops listing the
    // OUTER->INNER nesting breakdown (one-line summary only).
    void setShowContainerChainInTooltip(bool v) { m_showContainerChainInTooltip = v; }

    // When true, the entire row gets a faint background tint based on
    // its inferred direction (green = outbound, red = inbound).
    void setTintRowByDirection(bool v);

    // If true (and DNS resolution is on), this host's own iface addresses
    // are rendered as "localhost".
    void setResolveIfaceAddrsAsLocalhost(bool v) { m_agg.setResolveIfaceAddrsAsLocalhost(v); }

    // --- adaptive throughput gauge ---
    void setThroughputGaugeEnabled(bool v);
    void setThroughputMaxMode(ThroughputMaxMode m)
    {
        m_agg.setThroughputMaxMode(
            static_cast<qiftop::aggregate::ConnectionAggregator::ThroughputMaxMode>(m));
    }
    void setThroughputWindowMs(int ms) { m_agg.setThroughputWindowMs(ms); }
    void setRateSmoothingMs(int ms)    { m_agg.setRateSmoothingMs(ms); }
    void setPollIntervalMs(int ms)     { m_agg.setPollIntervalMs(ms); }

    [[nodiscard]] int      rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] int      columnCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    // Exportable
    [[nodiscard]] QStringList  exportHeaders() const override;
    [[nodiscard]] int          exportRowCount() const override;
    [[nodiscard]] QVariantList exportRow(int row) const override;

    // ----- helpers for the context-menu copy actions (row is a source row) ---
    enum class FlowEnd { Source, Destination };
    [[nodiscard]] QString copyTextForEndpoint(int row, FlowEnd which) const
    {
        return m_agg.copyTextForEndpoint(
            row, static_cast<qiftop::aggregate::ConnectionAggregator::FlowEnd>(which));
    }
    [[nodiscard]] QString copyTextForFlow(int row) const { return m_agg.copyTextForFlow(row); }
    [[nodiscard]] QString peerAddressText(int row) const { return m_agg.peerAddressText(row); }

public slots:
    void updateConnections(QList<Connection> connections) { m_agg.updateConnections(std::move(connections)); }
    void advanceSmoothing() { m_agg.advanceSmoothing(); }

private:
    using Row = qiftop::aggregate::ConnectionAggregator::Row;

    qiftop::aggregate::ConnectionAggregator m_agg;

    // Display-only flags that affect rendering in data() (colours, gauge,
    // tooltip detail) — not the underlying aggregation, so they stay here.
    bool m_tintRowByDirection          = false;
    bool m_gaugeEnabled                = false;
    bool m_showContainerChainInTooltip = true;
};
