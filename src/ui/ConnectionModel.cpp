#include "ConnectionModel.h"
#include "ConnectionHeuristics.h"
#include "dns/DnsResolver.h"
#include "util/Units.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QPalette>
#include <QRegularExpression>

#include <algorithm>

namespace {

// Read /proc/sys/net/ipv4/ip_local_port_range once. Returns (low, high);
// falls back to the kernel's compiled-in defaults if anything goes wrong.
std::pair<quint16, quint16> readEphemeralRange()
{
    QFile f(QStringLiteral("/proc/sys/net/ipv4/ip_local_port_range"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString s = QString::fromLatin1(f.readAll()).trimmed();
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        const QStringList parts = s.split(ws, Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            bool ok1 = false, ok2 = false;
            const int lo = parts[0].toInt(&ok1);
            const int hi = parts[1].toInt(&ok2);
            if (ok1 && ok2 && lo > 0 && hi >= lo && hi <= 65535)
                return {static_cast<quint16>(lo), static_cast<quint16>(hi)};
        }
    }
    return {32768, 60999};
}

} // namespace

ConnectionModel::ConnectionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_elapsed.start();
    std::tie(m_ephemeralLow, m_ephemeralHigh) = readEphemeralRange();
    m_loopbackAddrs.insert(QHostAddress(QHostAddress::LocalHost));
    m_loopbackAddrs.insert(QHostAddress(QHostAddress::LocalHostIPv6));
}

void ConnectionModel::setDnsResolver(DnsResolver *resolver)
{
    if (m_resolver == resolver) return;
    if (m_resolver)
        disconnect(m_resolver, nullptr, this, nullptr);
    m_resolver = resolver;
    if (m_resolver) {
        connect(m_resolver, &DnsResolver::resolved,
                this,       &ConnectionModel::onResolved);
    }
}

void ConnectionModel::setHostnameResolutionEnabled(bool enabled)
{
    if (m_resolveEnabled == enabled) return;
    m_resolveEnabled = enabled;
    if (!m_rows.isEmpty()) {
        emit dataChanged(index(0, static_cast<int>(Column::Flow)),
                         index(static_cast<int>(m_rows.size()) - 1,
                               static_cast<int>(Column::Flow)));
    }
    if (enabled && m_resolver) {
        for (const Row &r : std::as_const(m_rows)) {
            requestResolution(r.current.local.address);
            requestResolution(r.current.remote.address);
        }
    }
}

void ConnectionModel::setLocalAddresses(QSet<QHostAddress> addrs)
{
    // Loopback addresses live in m_loopbackAddrs (set once in the ctor);
    // m_localAddrs only ever contains this host's non-loopback iface IPs.
    addrs.remove(QHostAddress(QHostAddress::LocalHost));
    addrs.remove(QHostAddress(QHostAddress::LocalHostIPv6));
    if (addrs == m_localAddrs) return;
    m_localAddrs = std::move(addrs);
    // Repaint the flow column so any iface-IP currently shown as raw flips
    // to "localhost" (or back, if it was just removed).
    if (!m_rows.isEmpty() && m_resolveEnabled && m_aliasIfaceAddrsAsLocalhost) {
        emit dataChanged(index(0, static_cast<int>(Column::Flow)),
                         index(static_cast<int>(m_rows.size()) - 1,
                               static_cast<int>(Column::Flow)));
    }
}

int ConnectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int ConnectionModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Column::ColumnCount);
}

void ConnectionModel::setResolveIfaceAddrsAsLocalhost(bool v)
{
    if (v == m_aliasIfaceAddrsAsLocalhost) return;
    m_aliasIfaceAddrsAsLocalhost = v;
    if (!m_rows.isEmpty() && m_resolveEnabled) {
        emit dataChanged(index(0, static_cast<int>(Column::Flow)),
                         index(static_cast<int>(m_rows.size()) - 1,
                               static_cast<int>(Column::Flow)));
    }
}

void ConnectionModel::setTintRowByDirection(bool v)
{
    if (v == m_tintRowByDirection) return;
    m_tintRowByDirection = v;
    if (!m_rows.isEmpty()) {
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0),
                         index(static_cast<int>(m_rows.size()) - 1, lastCol),
                         {Qt::BackgroundRole});
    }
}

void ConnectionModel::setThroughputGaugeEnabled(bool v)
{
    if (v == m_gaugeEnabled) return;
    m_gaugeEnabled = v;
    if (!m_rows.isEmpty()) {
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0),
                         index(static_cast<int>(m_rows.size()) - 1, lastCol));
    }
}

void ConnectionModel::setThroughputMaxMode(ThroughputMaxMode m)
{
    if (m == m_maxMode) return;
    m_maxMode = m;
    if (!m_rows.isEmpty() && m_gaugeEnabled) {
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0),
                         index(static_cast<int>(m_rows.size()) - 1, lastCol));
    }
}

void ConnectionModel::setThroughputWindowMs(int ms)
{
    ms = ms < 1000 ? 1000 : ms;
    if (ms == m_windowMs) return;
    m_windowMs = ms;
    // Trim any now-out-of-window samples lazily on the next tick. Repaint
    // so the displayed Max columns reflect the new window immediately.
    if (!m_rows.isEmpty()
        && m_gaugeEnabled
        && m_maxMode == ThroughputMaxMode::Windowed)
    {
        const qint64 cutoff = m_lastElapsedMs - m_windowMs;
        for (Row &r : m_rows) {
            while (!r.rxSamples.empty() && r.rxSamples.front().timeMs < cutoff)
                r.rxSamples.pop_front();
            while (!r.txSamples.empty() && r.txSamples.front().timeMs < cutoff)
                r.txSamples.pop_front();
        }
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0),
                         index(static_cast<int>(m_rows.size()) - 1, lastCol));
    }
}

double ConnectionModel::rxReference(const Row &r) const
{
    if (m_maxMode == ThroughputMaxMode::CumulativeAverage)
        return r.samples > 0 ? r.rxSum / double(r.samples) : 0.0;
    double m = 0.0;
    for (const auto &s : r.rxSamples) if (s.rate > m) m = s.rate;
    return m;
}

double ConnectionModel::txReference(const Row &r) const
{
    if (m_maxMode == ThroughputMaxMode::CumulativeAverage)
        return r.samples > 0 ? r.txSum / double(r.samples) : 0.0;
    double m = 0.0;
    for (const auto &s : r.txSamples) if (s.rate > m) m = s.rate;
    return m;
}

bool ConnectionModel::isForwardedFlow(const Connection &c) const
{
    return qiftop::heuristics::isForwardedFlow(c, m_localAddrs, m_loopbackAddrs);
}

// How many samples a connection must accumulate before its adaptive
// reference (and therefore the gauge fraction / Max columns) is
// considered meaningful. The first 1–2 samples don't have enough
// history for the CMA to differ from the instantaneous reading, which
// would otherwise paint the gauge at ~100% on every new connection.
// Same gate is applied in Windowed mode for visual consistency.
static constexpr quint64 kGaugeWarmupSamples = 4;

QString ConnectionModel::displayHost(const QHostAddress &addr) const
{
    if (m_resolveEnabled) {
        if (m_loopbackAddrs.contains(addr))
            return QStringLiteral("localhost");
        if (m_aliasIfaceAddrsAsLocalhost && m_localAddrs.contains(addr))
            return QStringLiteral("localhost");
        if (m_resolver) {
            const QString cached = m_resolver->cachedName(addr);
            if (!cached.isEmpty())
                return cached;
        }
    }
    return addr.toString();
}

QString ConnectionModel::protoLabel(const Connection &c) const
{
    QString base = l4ProtoToString(c.proto);
    // With DNS resolution on the addresses are hostnames, so the family
    // is no longer visually obvious — tag TCP/UDP rows with v4/v6 so the
    // user can tell e.g. an A vs AAAA flow at a glance. ICMP rows already
    // carry the family in the name (ICMP vs ICMPv6).
    if (m_resolveEnabled
        && (c.proto == L4Proto::Tcp || c.proto == L4Proto::Udp))
    {
        const bool v6 = c.local.isIPv6() || c.remote.isIPv6();
        base += v6 ? QStringLiteral("v6") : QStringLiteral("v4");
    }
    return base;
}

QString ConnectionModel::endpointText(const Endpoint &ep) const
{
    // Port 0 is our sentinel for "aggregated / many ephemeral ports".
    const QString portStr = ep.port == 0 ? QStringLiteral("*")
                                          : QString::number(ep.port);
    const QString host = displayHost(ep.address);
    if (ep.address.protocol() == QAbstractSocket::IPv6Protocol
        && host == ep.address.toString())
    {
        return QStringLiteral("[%1]:%2").arg(host, portStr);
    }
    return QStringLiteral("%1:%2").arg(host, portStr);
}

QString ConnectionModel::endpointCopyText(const Endpoint &ep) const
{
    // Like endpointText, but: non-loopback iface addresses are emitted as
    // their numeric IP (never "localhost"), because pasting "localhost"
    // anywhere outside this host would resolve to a different machine.
    // Loopback (127.0.0.1/::1) stays as "localhost" when DNS is on.
    QString host;
    if (m_resolveEnabled && m_localAddrs.contains(ep.address)) {
        host = ep.address.toString();
    } else {
        host = displayHost(ep.address);
    }
    const QString portStr = ep.port == 0 ? QStringLiteral("*")
                                          : QString::number(ep.port);
    if (ep.address.protocol() == QAbstractSocket::IPv6Protocol
        && host == ep.address.toString())
    {
        return QStringLiteral("[%1]:%2").arg(host, portStr);
    }
    return QStringLiteral("%1:%2").arg(host, portStr);
}

QString ConnectionModel::copyTextForEndpoint(int row, FlowEnd which) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    // "Source" = initiator side, "Destination" = responder side. Without
    // a known direction we fall back to local→remote, matching the Flow
    // column's left→right rendering.
    const bool srcIsLocal =
        (c.direction != Direction::Inbound); // Outbound or Unknown
    const Endpoint &ep = (which == FlowEnd::Source)
        ? (srcIsLocal ? c.local  : c.remote)
        : (srcIsLocal ? c.remote : c.local);
    return endpointCopyText(ep);
}

QString ConnectionModel::copyTextForFlow(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    const QString src = copyTextForEndpoint(row, FlowEnd::Source);
    const QString dst = copyTextForEndpoint(row, FlowEnd::Destination);
    const QString iface = c.iface.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(c.iface);
    return QStringLiteral("%1  %2 → %3%4")
        .arg(protoLabel(c), src, dst, iface);
}

void ConnectionModel::requestResolution(const QHostAddress &addr)
{
    if (m_resolveEnabled && m_resolver && !addr.isNull())
        m_resolver->resolve(addr);
}

QVariant ConnectionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row  &row = m_rows[index.row()];
    const auto  col = static_cast<Column>(index.column());
    const Connection &c = row.current;

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Column::Iface:   return c.iface.isEmpty() ? QStringLiteral("—") : c.iface;
        case Column::Flow:
            return QStringLiteral("%1  %2 → %3")
                .arg(protoLabel(c), endpointText(c.local), endpointText(c.remote));
        case Column::RxRate:  return util::formatByteRate(row.rxRate);
        case Column::TxRate:  return util::formatByteRate(row.txRate);
        case Column::RxTotal: return util::formatBytes(c.rxBytes);
        case Column::TxTotal: return util::formatBytes(c.txBytes);
        case Column::RxMax: {
            if (row.samples < kGaugeWarmupSamples)
                return QStringLiteral("—");
            const double v = rxReference(row);
            return v > 0.0 ? util::formatByteRate(v) : QStringLiteral("—");
        }
        case Column::TxMax: {
            if (row.samples < kGaugeWarmupSamples)
                return QStringLiteral("—");
            const double v = txReference(row);
            return v > 0.0 ? util::formatByteRate(v) : QStringLiteral("—");
        }
        case Column::ColumnCount: break;
        }
        return {};

    case SortRole:
        switch (col) {
        case Column::Iface:   return c.iface;
        case Column::Flow:    return c.remote.address.toString();
        case Column::RxRate:  return row.rxRate;
        case Column::TxRate:  return row.txRate;
        case Column::RxTotal: return static_cast<qulonglong>(c.rxBytes);
        case Column::TxTotal: return static_cast<qulonglong>(c.txBytes);
        case Column::RxMax:   return rxReference(row);
        case Column::TxMax:   return txReference(row);
        case Column::ColumnCount: break;
        }
        return {};

    case IsIPv6Role:
        return c.local.isIPv6() || c.remote.isIPv6();
    case ProtoTextRole:
        return protoLabel(c);
    case ProtoRole:
        return static_cast<int>(c.proto);
    case LocalTextRole:
        return endpointText(c.local);
    case RemoteTextRole:
        return endpointText(c.remote);
    case IsStaleRole:
        return row.stale;
    case IfaceNameRole:
        return c.iface;
    case DirectionRole:
        return static_cast<int>(c.direction);
    case ConnectionRole:
        return QVariant::fromValue(c);
    case RxRateRole:
        return row.rxRate;
    case TxRateRole:
        return row.txRate;
    case HostnameLocalRole:
        return (m_resolveEnabled && m_resolver) ? m_resolver->cachedName(c.local.address)
                                                : QString();
    case HostnameRemoteRole:
        return (m_resolveEnabled && m_resolver) ? m_resolver->cachedName(c.remote.address)
                                                : QString();

    case GaugeFractionRole: {
        if (!m_gaugeEnabled)
            return {};
        // Warm-up: until we've collected a few samples, the per-row
        // reference is statistically meaningless (a 1-sample CMA equals
        // the instantaneous reading, so the gauge would lock to 100%
        // on every new flow). Return 0 so no dark portion paints.
        if (row.samples < kGaugeWarmupSamples)
            return 0.0;
        const double refRx = rxReference(row);
        const double refTx = txReference(row);
        const double ref   = refRx + refTx;
        if (ref <= 0.0)
            return 0.0;
        const double cur = row.rxRate + row.txRate;
        return qBound(0.0, cur / ref, 1.0);
    }
    case GaugeDarkColorRole: {
        if (!m_gaugeEnabled)
            return {};
        const QColor base = QApplication::palette().color(QPalette::Base);
        const bool   dark = base.lightness() < 128;
        // When direction tint is on, derive the gauge "fill" from the
        // direction color so the gauge reads as a saturated extension
        // of the row tint. Otherwise fall back to a neutral gray fill.
        if (m_tintRowByDirection) {
            if (isForwardedFlow(c))
                return dark ? QColor(130, 110, 40) : QColor(235, 215, 130);
            if (c.direction == Direction::Outbound)
                return dark ? QColor(60, 130, 60) : QColor(170, 220, 170);
            if (c.direction == Direction::Inbound)
                return dark ? QColor(140, 60, 60) : QColor(240, 180, 180);
        }
        return dark ? QColor(90, 90, 90) : QColor(190, 190, 190);
    }

    case Qt::FontRole: {
        QFont f;
        if (row.stale) f.setItalic(true);
        return f;
    }
    case Qt::ForegroundRole:
        if (row.stale)
            return QVariant::fromValue(QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        return {};

    case Qt::BackgroundRole:
        if (m_tintRowByDirection) {
            // Wireshark-ish: faint green for outbound, faint red for
            // inbound, faint yellow for forwarded (NAT/IP-forwarding
            // flows where neither end is this host). Theme-aware:
            // dark themes get desaturated overlays so the tint reads
            // without overpowering the row contents. When the
            // throughput gauge is on, this tint is the "unfilled"
            // portion — render it a little fainter so the gauge's
            // darker filled portion has visual room.
            const QColor baseCol = QApplication::palette().color(QPalette::Base);
            const bool   dark    = baseCol.lightness() < 128;
            if (isForwardedFlow(c)) {
                if (m_gaugeEnabled)
                    return dark ? QColor(45, 40, 22) : QColor(252, 248, 230);
                return dark ? QColor(60, 52, 28) : QColor(248, 240, 200);
            }
            if (c.direction == Direction::Outbound) {
                if (m_gaugeEnabled)
                    return dark ? QColor(28, 50, 28) : QColor(235, 250, 235);
                return dark ? QColor(36, 70, 36) : QColor(220, 245, 220);
            }
            if (c.direction == Direction::Inbound) {
                if (m_gaugeEnabled)
                    return dark ? QColor(50, 28, 28) : QColor(252, 235, 235);
                return dark ? QColor(70, 36, 36) : QColor(250, 220, 220);
            }
        }
        return {};

    case Qt::TextAlignmentRole:
        if (col == Column::Flow || col == Column::Iface)
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    return {};
}

QVariant ConnectionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (static_cast<Column>(section)) {
    case Column::Iface:   return tr("Iface");
    case Column::Flow:    return tr("Flow");
    case Column::RxRate:  return tr("RX rate");
    case Column::TxRate:  return tr("TX rate");
    case Column::RxTotal: return tr("RX total");
    case Column::TxTotal: return tr("TX total");
    case Column::RxMax:   return tr("Max RX");
    case Column::TxMax:   return tr("Max TX");
    case Column::ColumnCount: break;
    }
    return {};
}

QStringList ConnectionModel::exportHeaders() const
{
    return {
        QStringLiteral("iface"),
        QStringLiteral("proto"),
        QStringLiteral("localAddress"),
        QStringLiteral("localPort"),
        QStringLiteral("remoteAddress"),
        QStringLiteral("remotePort"),
        QStringLiteral("remoteHostname"),
        QStringLiteral("rxBytes"),
        QStringLiteral("txBytes"),
        QStringLiteral("rxPackets"),
        QStringLiteral("txPackets"),
        QStringLiteral("rxBytesPerSec"),
        QStringLiteral("txBytesPerSec"),
        QStringLiteral("rxBytesPerSecRef"),
        QStringLiteral("txBytesPerSecRef"),
    };
}

int ConnectionModel::exportRowCount() const
{
    return static_cast<int>(m_rows.size());
}

QVariantList ConnectionModel::exportRow(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row &r = m_rows[row];
    const Connection &c = r.current;
    const QString remoteName = (m_resolveEnabled && m_resolver)
        ? m_resolver->cachedName(c.remote.address)
        : QString();
    return {
        c.iface,
        l4ProtoToString(c.proto),
        c.local.address.toString(),
        c.local.port,
        c.remote.address.toString(),
        c.remote.port,
        remoteName,
        static_cast<qulonglong>(c.rxBytes),
        static_cast<qulonglong>(c.txBytes),
        static_cast<qulonglong>(c.rxPackets),
        static_cast<qulonglong>(c.txPackets),
        r.rxRate,
        r.txRate,
        rxReference(r),
        txReference(r),
    };
}

void ConnectionModel::updateConnections(QList<Connection> connections)
{
    const qint64 nowMs     = m_elapsed.elapsed();
    const qint64 deltaMs   = nowMs - m_lastElapsedMs;
    m_lastElapsedMs        = nowMs;
    const double deltaSecs = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // --- compute Direction client-side (see ConnectionHeuristics.h) ------
    for (Connection &c : connections) {
        c.direction = qiftop::heuristics::inferDirection(
            c, m_localAddrs, m_loopbackAddrs, m_ephemeralLow, m_ephemeralHigh);
    }

    // --- UDP aggregation by peer -----------------------------------------
    // Collapse the ephemeral side: outbound rows share remote.{addr,port}
    // and have local.port masked to 0 ("*"); inbound rows share local.{
    // addr,port} and have remote.port masked to 0. Direction::Unknown
    // flows are left alone (can't tell which port is ephemeral).
    if (m_udpAggregateByPeer) {
        QHash<QString, Connection> agg;
        QList<Connection>          passthrough;
        agg.reserve(connections.size());
        for (const Connection &c : std::as_const(connections)) {
            if (c.proto != L4Proto::Udp || c.direction == Direction::Unknown) {
                passthrough.append(c);
                continue;
            }
            Connection k = c;
            if (k.direction == Direction::Outbound) k.local.port  = 0;
            else                                    k.remote.port = 0;
            const QString aggKey = k.key();
            auto it = agg.find(aggKey);
            if (it == agg.end()) {
                agg.insert(aggKey, k);
            } else {
                it->rxBytes   += c.rxBytes;
                it->txBytes   += c.txBytes;
                it->rxPackets += c.rxPackets;
                it->txPackets += c.txPackets;
            }
        }
        connections = std::move(passthrough);
        connections.reserve(connections.size() + agg.size());
        for (auto it = agg.cbegin(); it != agg.cend(); ++it)
            connections.append(it.value());
    }

    // Index the (post-aggregation) snapshot by key so we can do row-level
    // updates instead of resetting the model every tick.
    QHash<QString, Connection> incoming;
    incoming.reserve(connections.size());
    for (const Connection &c : connections)
        incoming.insert(c.key(), c);

    // --- update existing rows / mark survivors as stale ---
    const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
    const qint64 windowCutoff = nowMs - m_windowMs;
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        const QString k = r.current.key();
        auto it = incoming.constFind(k);
        if (it != incoming.constEnd()) {
            const Connection &c = it.value();
            double rawRx = 0.0;
            double rawTx = 0.0;
            if (auto p = m_prev.constFind(k); p != m_prev.constEnd()) {
                // Guard against counter resets when conntrack recycles an
                // identical 5-tuple (rx/tx would otherwise go negative).
                rawRx = c.rxBytes >= p->rxBytes
                    ? static_cast<double>(c.rxBytes - p->rxBytes) / deltaSecs : 0.0;
                rawTx = c.txBytes >= p->txBytes
                    ? static_cast<double>(c.txBytes - p->txBytes) / deltaSecs : 0.0;
            }
            // Stash raw rates; reference (Max columns / gauge denom)
            // is computed from these so it doesn't inherit the
            // display-smoothing lag.
            r.rxRaw = rawRx;
            r.txRaw = rawTx;
            if (m_rateSmoothingMs > 0) {
                // Two-stage smoothing:
                //   1. Target = symmetric EMA of raw (noise rejection).
                //   2. Display tweens from current display to target
                //      with easeOutCubic over a poll-interval
                //      duration. Falls use ~1/3 the duration so drops
                //      feel snappier than spikes ("rates fall faster").
                if (deltaMs > 0) {
                    r.rxTarget = qiftop::heuristics::emaUpdate(
                        r.rxTarget, rawRx, double(deltaMs), double(m_rateSmoothingMs));
                    r.txTarget = qiftop::heuristics::emaUpdate(
                        r.txTarget, rawTx, double(deltaMs), double(m_rateSmoothingMs));
                } else {
                    r.rxTarget = rawRx;
                    r.txTarget = rawTx;
                }
                const int durRise = m_pollIntervalMs > 0 ? m_pollIntervalMs : 1000;
                const int durFall = std::max(100, durRise / 3);
                r.rxAnimFrom    = r.rxRate;
                r.rxAnimStartMs = nowMs;
                r.rxAnimDurMs   = (r.rxTarget < r.rxRate) ? durFall : durRise;
                r.txAnimFrom    = r.txRate;
                r.txAnimStartMs = nowMs;
                r.txAnimDurMs   = (r.txTarget < r.txRate) ? durFall : durRise;
            } else {
                r.rxRate    = rawRx;
                r.txRate    = rawTx;
                r.rxTarget  = rawRx;
                r.txTarget  = rawTx;
                r.rxAnimDurMs = 0;
                r.txAnimDurMs = 0;
            }
            // Adaptive throughput bookkeeping. Samples carry RAW rates
            // so the reference (Max columns / gauge denominator) is an
            // honest throughput measure rather than a smoothed one.
            r.rxSamples.push_back({nowMs, rawRx});
            r.txSamples.push_back({nowMs, rawTx});
            while (!r.rxSamples.empty() && r.rxSamples.front().timeMs < windowCutoff)
                r.rxSamples.pop_front();
            while (!r.txSamples.empty() && r.txSamples.front().timeMs < windowCutoff)
                r.txSamples.pop_front();
            r.rxSum += rawRx;
            r.txSum += rawTx;
            ++r.samples;

            r.current    = c;
            r.lastSeenMs = nowMs;
            r.stale      = false;
            emit dataChanged(index(int(i), 0), index(int(i), lastCol));
        } else if (!r.stale) {
            r.stale  = true;
            r.rxRate = 0.0;
            r.txRate = 0.0;
            r.rxRaw  = 0.0;
            r.txRaw  = 0.0;
            r.rxTarget = 0.0;
            r.txTarget = 0.0;
            r.rxAnimDurMs = 0;
            r.txAnimDurMs = 0;
            emit dataChanged(index(int(i), 0), index(int(i), lastCol));
        }
    }

    // --- prune rows that have been stale longer than the retention window ---
    // UDP gets its own retention budget (typically larger — UDP flows are
    // intrinsically bursty and the kernel times them out aggressively).
    for (qsizetype i = m_rows.size() - 1; i >= 0; --i) {
        const Row &r = m_rows[i];
        const int budget = (r.current.proto == L4Proto::Udp)
                         ? m_staleRetentionMsUdp
                         : m_staleRetentionMs;
        if (r.stale && nowMs - r.lastSeenMs > budget) {
            beginRemoveRows({}, int(i), int(i));
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }
    m_keyToIdx.clear();
    for (qsizetype i = 0; i < m_rows.size(); ++i)
        m_keyToIdx.insert(m_rows[i].current.key(), i);

    // --- append rows for previously-unseen flows ---
    QList<Connection> additions;
    for (auto it = incoming.constBegin(); it != incoming.constEnd(); ++it) {
        if (!m_keyToIdx.contains(it.key()))
            additions.append(it.value());
    }
    if (!additions.isEmpty()) {
        const int firstNew = int(m_rows.size());
        beginInsertRows({}, firstNew, firstNew + int(additions.size()) - 1);
        for (const Connection &c : std::as_const(additions)) {
            Row r;
            r.current    = c;
            r.lastSeenMs = nowMs;
            m_rows.append(std::move(r));
            m_keyToIdx.insert(c.key(), m_rows.size() - 1);
            if (m_resolveEnabled && m_resolver) {
                requestResolution(c.local.address);
                requestResolution(c.remote.address);
            }
        }
        endInsertRows();
    }

    m_prev = std::move(incoming);
}

void ConnectionModel::advanceSmoothing()
{
    if (m_rateSmoothingMs <= 0 || m_rows.isEmpty())
        return;
    const qint64 nowMs   = m_elapsed.elapsed();
    const int    lastCol = int(Column::ColumnCount) - 1;
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (r.stale) continue;
        bool moved = false;
        auto step = [&](double &disp, double &animFrom, qint64 &startMs,
                        int &durMs, double target) {
            if (durMs <= 0) return;
            const double t = double(nowMs - startMs) / double(durMs);
            if (t >= 1.0) {
                if (qAbs(disp - target) > 1.0) moved = true;
                disp   = target;
                durMs  = 0;            // animation done
                return;
            }
            if (t <= 0.0) return;
            const double eased = qiftop::heuristics::easeOutCubic(t);
            const double v = animFrom + (target - animFrom) * eased;
            if (qAbs(v - disp) > 1.0) moved = true;
            disp = v;
        };
        step(r.rxRate, r.rxAnimFrom, r.rxAnimStartMs, r.rxAnimDurMs, r.rxTarget);
        step(r.txRate, r.txAnimFrom, r.txAnimStartMs, r.txAnimDurMs, r.txTarget);
        if (moved)
            emit dataChanged(index(int(i), 0), index(int(i), lastCol));
    }
}

void ConnectionModel::onResolved(QHostAddress addr, QString /*hostname*/)
{
    if (!m_resolveEnabled || m_rows.isEmpty())
        return;

    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        const Connection &c = m_rows[i].current;
        if (c.local.address == addr || c.remote.address == addr) {
            emit dataChanged(index(static_cast<int>(i), static_cast<int>(Column::Flow)),
                             index(static_cast<int>(i), static_cast<int>(Column::Flow)),
                             {Qt::DisplayRole, LocalTextRole, RemoteTextRole});
        }
    }
}
