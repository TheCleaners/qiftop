#include "aggregate/ConnectionAggregator.h"

#include "backend/PlatformInfo.h"
#include "dns/DnsResolver.h"
#include "util/ConnectionHeuristics.h"

#include <algorithm>

namespace qiftop::aggregate {

ConnectionAggregator::ConnectionAggregator(QObject *parent)
    : QObject(parent)
{
    m_elapsed.start();
    std::tie(m_ephemeralLow, m_ephemeralHigh) = qiftop::platform::ephemeralPortRange();
    m_loopbackAddrs.insert(QHostAddress(QHostAddress::LocalHost));
    m_loopbackAddrs.insert(QHostAddress(QHostAddress::LocalHostIPv6));
}

void ConnectionAggregator::setDnsResolver(DnsResolver *resolver)
{
    if (m_resolver == resolver) return;
    if (m_resolver)
        disconnect(m_resolver, nullptr, this, nullptr);
    m_resolver = resolver;
    if (m_resolver) {
        connect(m_resolver, &DnsResolver::resolved,
                this,       &ConnectionAggregator::onResolved);
    }
}

void ConnectionAggregator::setHostnameResolutionEnabled(bool enabled)
{
    if (m_resolveEnabled == enabled) return;
    m_resolveEnabled = enabled;
    if (!m_rows.isEmpty())
        emit viewDataChanged();
    if (enabled && m_resolver) {
        for (const Row &r : std::as_const(m_rows)) {
            requestResolution(r.current.local.address);
            requestResolution(r.current.remote.address);
        }
    }
}

void ConnectionAggregator::setLocalAddresses(QSet<QHostAddress> addrs)
{
    // Loopback addresses live in m_loopbackAddrs (set once in the ctor);
    // m_localAddrs only ever contains this host's non-loopback iface IPs.
    addrs.remove(QHostAddress(QHostAddress::LocalHost));
    addrs.remove(QHostAddress(QHostAddress::LocalHostIPv6));
    if (addrs == m_localAddrs) return;
    m_localAddrs = std::move(addrs);
    if (!m_rows.isEmpty() && m_resolveEnabled && m_aliasIfaceAddrsAsLocalhost)
        emit viewDataChanged();
}

void ConnectionAggregator::setResolveIfaceAddrsAsLocalhost(bool v)
{
    if (v == m_aliasIfaceAddrsAsLocalhost) return;
    m_aliasIfaceAddrsAsLocalhost = v;
    if (!m_rows.isEmpty() && m_resolveEnabled)
        emit viewDataChanged();
}

void ConnectionAggregator::setThroughputMaxMode(ThroughputMaxMode m)
{
    if (m == m_maxMode) return;
    m_maxMode = m;
    if (!m_rows.isEmpty())
        emit viewDataChanged();
}

void ConnectionAggregator::setThroughputWindowMs(int ms)
{
    ms = ms < 1000 ? 1000 : ms;
    if (ms == m_windowMs) return;
    m_windowMs = ms;
    // Trim any now-out-of-window samples lazily on the next tick. Repaint
    // so the displayed Max columns reflect the new window immediately.
    if (!m_rows.isEmpty() && m_maxMode == ThroughputMaxMode::Windowed) {
        const qint64 cutoff = m_lastElapsedMs - m_windowMs;
        for (Row &r : m_rows) {
            while (!r.rxSamples.empty() && r.rxSamples.front().timeMs < cutoff)
                r.rxSamples.pop_front();
            while (!r.txSamples.empty() && r.txSamples.front().timeMs < cutoff)
                r.txSamples.pop_front();
        }
        emit viewDataChanged();
    }
}

double ConnectionAggregator::rxReference(const Row &r) const
{
    if (m_maxMode == ThroughputMaxMode::CumulativeAverage)
        return r.samples > 0 ? r.rxSum / double(r.samples) : 0.0;
    double m = 0.0;
    for (const auto &s : r.rxSamples) if (s.rate > m) m = s.rate;
    return m;
}

double ConnectionAggregator::txReference(const Row &r) const
{
    if (m_maxMode == ThroughputMaxMode::CumulativeAverage)
        return r.samples > 0 ? r.txSum / double(r.samples) : 0.0;
    double m = 0.0;
    for (const auto &s : r.txSamples) if (s.rate > m) m = s.rate;
    return m;
}

bool ConnectionAggregator::isForwardedFlow(const Connection &c) const
{
    return qiftop::heuristics::isForwardedFlow(c, m_localAddrs, m_loopbackAddrs);
}

QString ConnectionAggregator::displayHost(const QHostAddress &addr) const
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

QString ConnectionAggregator::cachedHostname(const QHostAddress &addr) const
{
    return (m_resolveEnabled && m_resolver) ? m_resolver->cachedName(addr)
                                            : QString();
}

QString ConnectionAggregator::protoLabel(const Connection &c) const
{
    QString base = l4ProtoToString(c.proto);
    // With DNS resolution on the addresses are hostnames, so the family
    // is no longer visually obvious — tag TCP/UDP rows with v4/v6 so the
    // user can tell e.g. an A vs AAAA flow at a glance.
    if (m_resolveEnabled
        && (c.proto == L4Proto::Tcp || c.proto == L4Proto::Udp))
    {
        const bool v6 = c.local.isIPv6() || c.remote.isIPv6();
        base += v6 ? QStringLiteral("v6") : QStringLiteral("v4");
    }
    return base;
}

QString ConnectionAggregator::endpointText(const Endpoint &ep) const
{
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

QString ConnectionAggregator::endpointCopyText(const Endpoint &ep) const
{
    // Like endpointText, but non-loopback iface addresses are emitted as
    // their numeric IP (never "localhost").
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

QString ConnectionAggregator::copyTextForEndpoint(int row, FlowEnd which) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    const bool srcIsLocal = (c.direction != Direction::Inbound);
    const Endpoint &ep = (which == FlowEnd::Source)
        ? (srcIsLocal ? c.local  : c.remote)
        : (srcIsLocal ? c.remote : c.local);
    return endpointCopyText(ep);
}

QString ConnectionAggregator::copyTextForFlow(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    const QString src = copyTextForEndpoint(row, FlowEnd::Source);
    const QString dst = copyTextForEndpoint(row, FlowEnd::Destination);
    const QString iface = c.iface.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(c.iface);
    return QStringLiteral("%1  %2 → %3%4")
        .arg(protoLabel(c), src, dst, iface);
}

QString ConnectionAggregator::peerAddressText(int row) const
{
    if (row < 0 || row >= m_rows.size()) return {};
    const Connection &c = m_rows[row].current;
    const Endpoint &peer = (c.direction == Direction::Inbound) ? c.local : c.remote;
    if (peer.address.isNull()) return {};
    return peer.address.toString();
}

void ConnectionAggregator::requestResolution(const QHostAddress &addr)
{
    if (m_resolveEnabled && m_resolver && !addr.isNull())
        m_resolver->resolve(addr);
}

void ConnectionAggregator::updateConnections(QList<Connection> connections)
{
    const qint64 nowMs     = m_elapsed.elapsed();
    const qint64 deltaMs   = nowMs - m_lastElapsedMs;
    m_lastElapsedMs        = nowMs;
    const double deltaSecs = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // --- compute Direction (heuristic fallback when wire says Unknown) ---
    for (Connection &c : connections) {
        if (c.direction != Direction::Unknown) continue;
        c.direction = qiftop::heuristics::inferDirection(
            c, m_localAddrs, m_loopbackAddrs, m_ephemeralLow, m_ephemeralHigh);
    }

    // --- UDP aggregation by peer ---
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
            const QString aggKey    = k.key();
            const QString memberKey = c.key();
            UdpAggState &st = m_udpAgg[aggKey];
            st.lastSeenMs = nowMs;
            UdpAggMember &mem = st.members[memberKey];
            if (c.rxBytes < mem.rxBytes || c.txBytes < mem.txBytes) {
                st.rxBase    += mem.rxBytes;   st.txBase    += mem.txBytes;
                st.rxPktBase += mem.rxPackets; st.txPktBase += mem.txPackets;
            }
            mem.rxBytes    = c.rxBytes;   mem.txBytes    = c.txBytes;
            mem.rxPackets  = c.rxPackets; mem.txPackets  = c.txPackets;
            mem.lastSeenMs = nowMs;
            if (!agg.contains(aggKey))
                agg.insert(aggKey, k);
        }

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

        for (auto it = agg.begin(); it != agg.end(); ++it) {
            const auto stIt = m_udpAgg.constFind(it.key());
            if (stIt == m_udpAgg.constEnd()) continue;
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

    // Index the (post-aggregation) snapshot by key.
    QHash<QString, Connection> incoming;
    incoming.reserve(connections.size());
    for (const Connection &c : connections)
        incoming.insert(c.key(), c);

    // --- update existing rows / mark survivors as stale ---
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
                rawRx = c.rxBytes >= p->rxBytes
                    ? static_cast<double>(c.rxBytes - p->rxBytes) / deltaSecs : 0.0;
                rawTx = c.txBytes >= p->txBytes
                    ? static_cast<double>(c.txBytes - p->txBytes) / deltaSecs : 0.0;
            }
            r.rxRaw = rawRx;
            r.txRaw = rawTx;
            if (m_rateSmoothingMs > 0) {
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
            emit rowsUpdated(int(i), int(i));
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
            emit rowsUpdated(int(i), int(i));
        }
    }

    // --- prune rows that have been stale longer than the retention window ---
    for (qsizetype i = m_rows.size() - 1; i >= 0; --i) {
        const Row &r = m_rows[i];
        const int budget = (r.current.proto == L4Proto::Udp)
                         ? m_staleRetentionMsUdp
                         : m_staleRetentionMs;
        if (r.stale && nowMs - r.lastSeenMs > budget) {
            emit rowsAboutToBeRemoved(int(i), int(i));
            m_rows.removeAt(i);
            emit rowsRemoved();
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
        emit rowsAboutToBeInserted(firstNew, firstNew + int(additions.size()) - 1);
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
        emit rowsInserted();
    }

    m_prev = std::move(incoming);
}

void ConnectionAggregator::advanceSmoothing()
{
    if (m_rateSmoothingMs <= 0 || m_rows.isEmpty())
        return;
    const qint64 nowMs = m_elapsed.elapsed();
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
                durMs  = 0;
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
            emit rowsUpdated(int(i), int(i));
    }
}

void ConnectionAggregator::onResolved(QHostAddress addr, QString /*hostname*/)
{
    if (!m_resolveEnabled || m_rows.isEmpty())
        return;

    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        const Connection &c = m_rows[i].current;
        if (c.local.address == addr || c.remote.address == addr)
            emit rowsUpdated(int(i), int(i));
    }
}

} // namespace qiftop::aggregate
