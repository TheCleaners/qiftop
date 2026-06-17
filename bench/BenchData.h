#pragma once

#include "backend/Connection.h"
#include "backend/NetworkMonitor.h"

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>

namespace qiftop::bench {

inline constexpr qsizetype kSize1K = 1'000;
inline constexpr qsizetype kSizeCap = 4'096;
inline constexpr qsizetype kSize10K = 10'000;
inline constexpr qsizetype kSize100K = 100'000;

struct FlowOptions {
    qsizetype count = kSizeCap;
    int tick = 0;
    double udpRatio = 0.25;
    double ipv6Ratio = 0.10;
    double attributedRatio = 0.50;
    double containerRatio = 0.25;
    bool includeContainerChains = true;
};

namespace detail {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}

    [[nodiscard]] std::uint64_t next()
    {
        std::uint64_t z = (m_state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    [[nodiscard]] qsizetype bounded(qsizetype n)
    {
        return n <= 0 ? 0 : static_cast<qsizetype>(next() % static_cast<std::uint64_t>(n));
    }

    [[nodiscard]] bool chance(double ratio)
    {
        ratio = std::clamp(ratio, 0.0, 1.0);
        constexpr double denom = double(1ULL << 53U);
        const double unit = double(next() >> 11U) / denom;
        return unit < ratio;
    }

private:
    std::uint64_t m_state;
};

[[nodiscard]] inline QHostAddress ipv4(quint32 prefix, qsizetype i)
{
    const int a = int((prefix >> 8U) & 0xffU);
    const int b = int(prefix & 0xffU);
    const int c = int((i / 254) % 254) + 1;
    const int d = int(i % 254) + 1;
    return QHostAddress(QStringLiteral("10.%1.%2.%3").arg(a).arg(c).arg(d + b > 254 ? d : d + b));
}

[[nodiscard]] inline QHostAddress remoteIpv4(qsizetype i)
{
    const int b = int((i / (qsizetype(254) * 254)) % 254) + 1;
    const int c = int((i / 254) % 254) + 1;
    const int d = int(i % 254) + 1;
    return QHostAddress(QStringLiteral("203.%1.%2.%3").arg(b).arg(c).arg(d));
}

[[nodiscard]] inline QHostAddress ipv6(qsizetype i)
{
    return QHostAddress(QStringLiteral("2001:db8:%1:%2::%3")
        .arg(qulonglong((i >> 16) & 0xffff), 0, 16)
        .arg(qulonglong(i & 0xffff), 0, 16)
        .arg(qulonglong((i % 0xfffe) + 1), 0, 16));
}

[[nodiscard]] inline QString hexId(qsizetype i)
{
    return QStringLiteral("%1").arg(qulonglong(0x100000000000ULL + std::uint64_t(i) * 2654435761ULL),
                                   12, 16, QLatin1Char('0')).right(12);
}

} // namespace detail

[[nodiscard]] inline QList<Connection> makeConnections(const FlowOptions &options)
{
    QList<Connection> out;
    out.reserve(options.count);

    detail::Rng rng(0x51f70f5eedULL
                    ^ (std::uint64_t(options.count) << 17U)
                    ^ (std::uint64_t(std::max(options.tick, 0)) << 33U));
    constexpr std::array<const char *, 6> kComms = {
        "nginx", "sshd", "curl", "qiftop-agent", "postgres", "redis"
    };
    constexpr std::array<const char *, 4> kRuntimes = {
        "docker", "containerd", "podman", "kubernetes"
    };
    constexpr std::array<const char *, 5> kIfaces = {
        "eth0", "wlan0", "enp1s0", "br0", "veth42"
    };

    for (qsizetype i = 0; i < options.count; ++i) {
        Connection c;
        const bool udp = rng.chance(options.udpRatio);
        const bool icmp = !udp && (i % 37 == 0);
        const bool ipv6 = rng.chance(options.ipv6Ratio);
        c.proto = icmp ? (ipv6 ? L4Proto::IcmpV6 : L4Proto::Icmp)
                       : (udp ? L4Proto::Udp : L4Proto::Tcp);
        c.direction = (i % 5 == 0) ? Direction::Inbound
                    : (i % 17 == 0) ? Direction::Unknown
                                    : Direction::Outbound;
        c.tcpState = c.proto == L4Proto::Tcp
            ? (i % 23 == 0 ? TcpState::TimeWait : TcpState::Established)
            : TcpState::None;
        c.reason = (i % 29 == 0) ? AttributionReason::Forwarded
                 : (i % 23 == 0) ? AttributionReason::Orphaned
                                  : AttributionReason::Resolved;

        c.local.address = ipv6 ? detail::ipv6(i) : detail::ipv4(0x0100U, i);
        c.remote.address = ipv6 ? detail::ipv6(options.count + i) : detail::remoteIpv4(options.count + i);
        c.local.port = c.proto == L4Proto::Icmp || c.proto == L4Proto::IcmpV6
            ? 0
            : quint16(10'000 + (i % 50'000));
        c.remote.port = c.proto == L4Proto::Udp
            ? quint16((i % 3 == 0) ? 53 : 443)
            : quint16((i % 7 == 0) ? 22 : 443);

        const quint64 base = quint64(i + 1) * 97ULL + quint64(options.tick) * 4096ULL;
        c.rxBytes = base + quint64((rng.next() & 0xfffU) + quint64(options.tick) * 512);
        c.txBytes = base / 2ULL + quint64((rng.next() & 0x7ffU) + quint64(options.tick) * 768);
        c.rxPackets = c.rxBytes / 1300ULL + 1ULL;
        c.txPackets = c.txBytes / 1300ULL + 1ULL;
        c.iface = QString::fromLatin1(kIfaces[std::size_t(i % qsizetype(kIfaces.size()))]);
        c.ifIndex = quint32(2 + (i % 64));

        if (rng.chance(options.attributedRatio)) {
            c.process.pid = qint32(1000 + (i % 60'000));
            c.process.uid = quint32((i % 8 == 0) ? 0 : 1000 + (i % 32));
            c.process.comm = QString::fromLatin1(kComms[std::size_t(i % qsizetype(kComms.size()))]);

            if (rng.chance(options.containerRatio)) {
                const QString runtime = QString::fromLatin1(
                    kRuntimes[std::size_t(i % qsizetype(kRuntimes.size()))]);
                c.container = qiftop::backend::ContainerInfo{
                    runtime,
                    detail::hexId(i),
                    QStringLiteral("workload-%1").arg(i % 1024),
                };
                if (options.includeContainerChains) {
                    if (i % 3 == 0) {
                        c.containerChain = {
                            qiftop::backend::ContainerInfo{
                                QStringLiteral("kubernetes"),
                                QStringLiteral("pod-%1").arg(i % 512),
                                QString{}},
                            c.container,
                        };
                    } else {
                        c.containerChain = {c.container};
                    }
                }
            }
        } else {
            c.reason = (i % 2 == 0) ? AttributionReason::NoLocalSocket
                                    : AttributionReason::Forwarded;
        }

        out.append(std::move(c));
    }
    return out;
}

[[nodiscard]] inline QList<Connection> bumpCounters(QList<Connection> previous, int tickDelta = 1)
{
    const quint64 dt = quint64(std::max(tickDelta, 1));
    for (qsizetype i = 0; i < previous.size(); ++i) {
        Connection &c = previous[i];
        const quint64 rxInc = dt * (1024ULL + quint64((i * 131) % 65'536));
        const quint64 txInc = dt * (768ULL + quint64((i * 197) % 49'152));
        c.rxBytes += rxInc;
        c.txBytes += txInc;
        c.rxPackets += rxInc / 1300ULL + 1ULL;
        c.txPackets += txInc / 1300ULL + 1ULL;
    }
    return previous;
}

[[nodiscard]] inline QList<InterfaceStats> makeInterfaces(qsizetype count, int tick = 0)
{
    QList<InterfaceStats> out;
    out.reserve(count);
    constexpr std::array<const char *, 5> kTypes = {
        "ethernet", "veth", "bridge", "vlan", "tun"
    };

    for (qsizetype i = 0; i < count; ++i) {
        InterfaceStats s;
        s.name = QStringLiteral("if%1").arg(i);
        s.type = QString::fromLatin1(kTypes[std::size_t(i % qsizetype(kTypes.size()))]);
        s.mtu = quint32((i % 5 == 0) ? 9000 : 1500);
        s.addresses = {
            QStringLiteral("10.%1.%2.1/24").arg((i / 254) % 254).arg(i % 254),
            QStringLiteral("2001:db8:%1::1/64").arg(qulonglong(i & 0xffff), 0, 16),
        };
        s.rxBytes = quint64(i + 1) * 10'000ULL + quint64(tick) * 4096ULL;
        s.txBytes = quint64(i + 1) * 8'000ULL + quint64(tick) * 3072ULL;
        s.rxPackets = s.rxBytes / 1500ULL + 1ULL;
        s.txPackets = s.txBytes / 1500ULL + 1ULL;
        s.isUp = i % 11 != 0;
        s.isLoopback = i == 0;
        s.ifIndex = quint32(i + 1);
        s.operState = s.isUp ? quint8(6) : quint8(2);
        s.rxErrors = quint64(i % 13);
        s.txErrors = quint64(i % 17);
        s.rxDropped = quint64(i % 19);
        s.txDropped = quint64(i % 23);
        out.append(std::move(s));
    }
    return out;
}

} // namespace qiftop::bench
