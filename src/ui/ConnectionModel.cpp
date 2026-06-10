#include "ConnectionModel.h"
#include "util/ConnectionHeuristics.h"
#include "backend/PlatformInfo.h"
#include "dns/DnsResolver.h"
#include "util/Units.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

#include <algorithm>

namespace {

} // namespace

ConnectionModel::ConnectionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_elapsed.start();
    std::tie(m_ephemeralLow, m_ephemeralHigh) = qiftop::platform::ephemeralPortRange();
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

QString ConnectionModel::peerAddressText(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    // Peer = the non-local end. With an unknown direction we fall back to
    // `remote`, matching how copyTextForEndpoint(Destination) renders for
    // unknown-direction outbound-style flows.
    const Endpoint &peer = (c.direction == Direction::Inbound) ? c.local : c.remote;
    if (peer.address.isNull()) return {};
    return peer.address.toString();
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
        case Column::Process:
            if (c.process.pid > 0) {
                const QString name = c.process.comm.isEmpty()
                                         ? QStringLiteral("pid %1").arg(c.process.pid)
                                         : c.process.comm;
                return QStringLiteral("%1  [%2]").arg(name).arg(c.process.pid);
            }
            return QStringLiteral("—");
        case Column::Container: {
            const auto &ci = c.container;
            if (ci.runtime.isEmpty() && ci.name.isEmpty() && ci.id.isEmpty())
                return QStringLiteral("(host)");
            const QString display = !ci.name.isEmpty() ? ci.name : ci.id.left(12);
            const QString primary = ci.runtime.isEmpty()
                                        ? display
                                        : QStringLiteral("%1:%2").arg(ci.runtime, display);
            if (c.containerChain.size() >= 2)
                return QStringLiteral("%1  ▸").arg(primary);
            return primary;
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
        case Column::Process: return static_cast<qulonglong>(c.process.pid);
        case Column::Container: {
            // Sort key: prefer name, fall back to id; never empty so
            // (host) rows clump together at the top/bottom rather than
            // scattering.
            const auto &ci = c.container;
            if (!ci.name.isEmpty()) return ci.name;
            if (!ci.id.isEmpty())   return ci.id;
            return QStringLiteral("\u0001(host)"); // sort to one end
        }
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
    case ProcessPidRole:
        return c.process.pid;
    case ProcessCommRole:
        return c.process.comm;
    case ContainerRuntimeRole:
        return c.container.runtime;
    case ContainerIdRole:
        return c.container.id;
    case ContainerNameRole:
        return c.container.name;
    case ContainerChainRole: {
        QStringList out;
        out.reserve(c.containerChain.size());
        for (const auto &ci : c.containerChain) {
            const QString display = !ci.name.isEmpty() ? ci.name : ci.id.left(12);
            out << (ci.runtime.isEmpty()
                        ? display
                        : QStringLiteral("%1:%2").arg(ci.runtime, display));
        }
        return out;
    }

    case Qt::ToolTipRole: {
        // Tooltip injection hardening (M2): Qt auto-detects rich text in
        // tooltips, and comm / container runtime/id/name are attacker-
        // controlled (prctl(PR_SET_NAME), image labels...). Escape every
        // dynamic field and render the tooltip as DELIBERATE rich text
        // (<qt> + <br>) so the escaping displays correctly.
        const auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
        switch (col) {
        case Column::Process:
            if (c.process.pid <= 0) return QStringLiteral("Unattributed flow");
            return QStringLiteral("<qt>pid: %1<br>comm: %2<br>uid: %3<br><br>Right-click → Details for cmdline / exe / cwd</qt>")
                .arg(c.process.pid)
                .arg(c.process.comm.isEmpty() ? QStringLiteral("?")
                                              : esc(c.process.comm))
                .arg(c.process.uid);
        case Column::Container: {
            if (c.container.runtime.isEmpty() && c.container.id.isEmpty()
                && c.container.name.isEmpty()) {
                return QStringLiteral("Host process (no container)");
            }
            QString s = QStringLiteral("runtime: %1<br>id: %2<br>name: %3")
                .arg(c.container.runtime.isEmpty() ? QStringLiteral("?") : esc(c.container.runtime),
                     c.container.id.isEmpty()      ? QStringLiteral("?") : esc(c.container.id),
                     c.container.name.isEmpty()    ? QStringLiteral("?") : esc(c.container.name));
            if (c.containerChain.size() >= 2 && m_showContainerChainInTooltip) {
                s += QStringLiteral("<br><br>Nesting (outer → inner):");
                for (const auto &ci : c.containerChain) {
                    const QString disp = !ci.name.isEmpty() ? ci.name
                                                            : ci.id.left(12);
                    s += QStringLiteral("<br>&nbsp;&nbsp;• %1:%2")
                             .arg(ci.runtime.isEmpty() ? QStringLiteral("?") : esc(ci.runtime),
                                  esc(disp));
                }
            }
            return QStringLiteral("<qt>%1</qt>").arg(s);
        }
        default: return {};
        }
    }

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
    case Column::Process: return tr("Process");
    case Column::Container: return tr("Container");
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
        QStringLiteral("pid"),
        QStringLiteral("uid"),
        QStringLiteral("comm"),
        QStringLiteral("containerRuntime"),
        QStringLiteral("containerId"),
        QStringLiteral("containerName"),
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
        c.process.pid,
        c.process.uid,
        c.process.comm,
        c.container.runtime,
        c.container.id,
        c.container.name,
    };
}

void ConnectionModel::updateConnections(QList<Connection> connections)
{
    const qint64 nowMs     = m_elapsed.elapsed();
    const qint64 deltaMs   = nowMs - m_lastElapsedMs;
    m_lastElapsedMs        = nowMs;
    const double deltaSecs = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // --- compute Direction --------------------------------------------------
    // Server (qiftop-agent ≥ 0.2) ships direction on the wire (capability
    // token "direction-on-wire"). When the in-process backend or an older
    // agent leaves it as Unknown, fall back to the local heuristic so
    // ephemeral-port inference still works.
    for (Connection &c : connections) {
        if (c.direction != Direction::Unknown) continue;
        c.direction = qiftop::heuristics::inferDirection(
            c, m_localAddrs, m_loopbackAddrs, m_ephemeralLow, m_ephemeralHigh);
    }

    // --- UDP aggregation by peer -----------------------------------------
    // Collapse the ephemeral side: outbound rows share remote.{addr,port}
    // and have local.port masked to 0 ("*"); inbound rows share local.{
    // addr,port} and have remote.port masked to 0. Direction::Unknown
    // flows are left alone (can't tell which port is ephemeral).
    //
    // Aggregate counters come from retained per-member state (m_udpAgg),
    // NOT from summing the current snapshot: UDP conntrack entries are
    // timed out aggressively, and a member dropping out of the snapshot
    // must not subtract its accumulated bytes from the aggregate (the
    // total would regress and the m_prev rate diff would clamp to 0).
    if (m_udpAggregateByPeer) {
        QHash<QString, Connection> agg;   // aggKey → representative (masked) flow
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
            const QString aggKey    = k.key();
            const QString memberKey = c.key();
            UdpAggState &st = m_udpAgg[aggKey];
            st.lastSeenMs = nowMs;
            UdpAggMember &mem = st.members[memberKey];
            if (c.rxBytes < mem.rxBytes || c.txBytes < mem.txBytes) {
                // Conntrack recycled the member tuple (counters reset):
                // fold the previous incarnation's high-water mark into
                // the base so the aggregate never regresses.
                st.rxBase    += mem.rxBytes;   st.txBase    += mem.txBytes;
                st.rxPktBase += mem.rxPackets; st.txPktBase += mem.txPackets;
            }
            mem.rxBytes    = c.rxBytes;   mem.txBytes    = c.txBytes;
            mem.rxPackets  = c.rxPackets; mem.txPackets  = c.txPackets;
            mem.lastSeenMs = nowMs;
            if (!agg.contains(aggKey))
                agg.insert(aggKey, k);    // counters materialised below
        }

        // Compact retained state: aggregates unseen beyond the UDP
        // retention window are dropped wholesale (their model row has
        // been pruned by then — a reappearing peer starts a fresh row);
        // long-expired members are folded into the base and erased so
        // the per-aggregate member map stays bounded under ephemeral-
        // port churn. Members absent for LESS than the window keep
        // their last counters in the sum — so a freshly expired member
        // never drops bytes, and a member that merely flickered out of
        // a capped snapshot isn't double-counted on return.
        for (auto it = m_udpAgg.begin(); it != m_udpAgg.end();) {
            UdpAggState &st = it.value();
            if (nowMs - st.lastSeenMs > m_staleRetentionMsUdp) {
                it = m_udpAgg.erase(it);
                continue;
            }
            for (auto mIt = st.members.begin(); mIt != st.members.end();) {
                if (nowMs - mIt->lastSeenMs > m_staleRetentionMsUdp) {
                    st.rxBase    += mIt->rxBytes;   st.txBase    += mIt->txBytes;
                    st.rxPktBase += mIt->rxPackets; st.txPktBase += mIt->txPackets;
                    mIt = st.members.erase(mIt);
                } else {
                    ++mIt;
                }
            }
            ++it;
        }

        // Materialise the aggregate counters: base + Σ retained members.
        for (auto it = agg.begin(); it != agg.end(); ++it) {
            const auto stIt = m_udpAgg.constFind(it.key());
            if (stIt == m_udpAgg.constEnd()) continue;  // pruned above (retention 0)
            quint64 rx = stIt->rxBase,    tx = stIt->txBase;
            quint64 rxp = stIt->rxPktBase, txp = stIt->txPktBase;
            for (const UdpAggMember &m : stIt->members) {
                rx  += m.rxBytes;   tx  += m.txBytes;
                rxp += m.rxPackets; txp += m.txPackets;
            }
            it->rxBytes   = rx;  it->txBytes   = tx;
            it->rxPackets = rxp; it->txPackets = txp;
        }

        connections = std::move(passthrough);
        connections.reserve(connections.size() + agg.size());
        for (auto it = agg.cbegin(); it != agg.cend(); ++it)
            connections.append(it.value());
    } else if (!m_udpAgg.isEmpty()) {
        m_udpAgg.clear();
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
