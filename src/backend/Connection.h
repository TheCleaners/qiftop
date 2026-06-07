#pragma once

#include <QHostAddress>
#include <QList>
#include <QMetaType>

#include <compare>

enum class L4Proto : quint8 {
    Unknown = 0,
    Tcp,
    Udp,
    Icmp,
    IcmpV6,
};

// Best-effort directionality of a flow relative to this host. Set by the
// client-side model (NOT by the agent wire format) using an ephemeral-port
// heuristic against /proc/sys/net/ipv4/ip_local_port_range so we can keep
// the DBus contract unchanged.
enum class Direction : quint8 {
    Unknown = 0,    // can't tell (e.g. forwarded, or both ports ephemeral)
    Outbound,       // local end picked the ephemeral port
    Inbound,        // remote end picked the ephemeral port (we're listening)
};

[[nodiscard]] inline QString l4ProtoToString(L4Proto p)
{
    switch (p) {
    case L4Proto::Tcp:     return QStringLiteral("TCP");
    case L4Proto::Udp:     return QStringLiteral("UDP");
    case L4Proto::Icmp:    return QStringLiteral("ICMP");
    case L4Proto::IcmpV6:  return QStringLiteral("ICMPv6");
    case L4Proto::Unknown: break;
    }
    return QStringLiteral("?");
}

struct Endpoint {
    QHostAddress address;
    quint16      port = 0;

    [[nodiscard]] bool isIPv6() const { return address.protocol() == QAbstractSocket::IPv6Protocol; }

    friend bool operator==(const Endpoint &, const Endpoint &) = default;
};

// A unidirectional flow descriptor. Local/remote disambiguation is the
// responsibility of the backend (typically by matching against local interface
// addresses or conntrack ORIGINAL/REPLY tuples).
struct Connection {
    Endpoint local;
    Endpoint remote;
    L4Proto  proto = L4Proto::Unknown;

    quint64 rxBytes   = 0; // remote -> local
    quint64 txBytes   = 0; // local  -> remote
    quint64 rxPackets = 0;
    quint64 txPackets = 0;

    // Best-effort egress interface (name, e.g. "wlp228s0"). Empty when the
    // backend couldn't attribute the flow (e.g. forwarded with no matching
    // route, or determination is platform-unsupported).
    QString iface;

    // Transient, computed client-side (see Direction). Not on the wire.
    Direction direction = Direction::Unknown;

    // Canonical key used by models to identify a flow across updates.
    // Includes direction so aggregated inbound/outbound rows for the same
    // peer never collide.
    [[nodiscard]] QString key() const
    {
        return QStringLiteral("%1|%2|%3.%4|%5.%6")
            .arg(static_cast<int>(proto))
            .arg(static_cast<int>(direction))
            .arg(local.address.toString()).arg(local.port)
            .arg(remote.address.toString()).arg(remote.port);
    }
};

Q_DECLARE_METATYPE(Connection)
Q_DECLARE_METATYPE(QList<Connection>)
