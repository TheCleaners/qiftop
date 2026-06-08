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

// IANA protocol numbers (RFC 5237). Used on the DBus wire so non-Qt
// clients can decode `proto` without knowing our internal L4Proto enum.
namespace l4proto::iana {
inline constexpr quint8 kIcmp   = 1;
inline constexpr quint8 kTcp    = 6;
inline constexpr quint8 kUdp    = 17;
inline constexpr quint8 kIcmpV6 = 58;
}

[[nodiscard]] inline quint8 toIanaProto(L4Proto p)
{
    switch (p) {
    case L4Proto::Tcp:    return l4proto::iana::kTcp;
    case L4Proto::Udp:    return l4proto::iana::kUdp;
    case L4Proto::Icmp:   return l4proto::iana::kIcmp;
    case L4Proto::IcmpV6: return l4proto::iana::kIcmpV6;
    case L4Proto::Unknown: break;
    }
    return 0;
}

[[nodiscard]] inline L4Proto fromIanaProto(quint8 n)
{
    switch (n) {
    case l4proto::iana::kTcp:    return L4Proto::Tcp;
    case l4proto::iana::kUdp:    return L4Proto::Udp;
    case l4proto::iana::kIcmp:   return L4Proto::Icmp;
    case l4proto::iana::kIcmpV6: return L4Proto::IcmpV6;
    default:                     return L4Proto::Unknown;
    }
}

// Best-effort directionality of a flow relative to this host. Set by the
// client-side model (NOT by the agent wire format) using an ephemeral-port
// heuristic against /proc/sys/net/ipv4/ip_local_port_range so we can keep
// the DBus contract unchanged.
enum class Direction : quint8 {
    Unknown = 0,    // can't tell (e.g. forwarded, or both ports ephemeral)
    Outbound,       // local end picked the ephemeral port
    Inbound,        // remote end picked the ephemeral port (we're listening)
};

// Connection-tracking TCP state. Mirrors the values from
// <linux/netfilter/nf_conntrack_tcp.h> (TCP_CONNTRACK_*) so libqiftop
// consumers can decode without pulling in kernel headers. Non-TCP flows
// always report `None`.
enum class TcpState : quint8 {
    None        = 0,
    SynSent     = 1,
    SynRecv     = 2,
    Established = 3,
    FinWait     = 4,
    CloseWait   = 5,
    LastAck     = 6,
    TimeWait    = 7,
    Close       = 8,
    SynSent2    = 9,
};

[[nodiscard]] inline QString tcpStateToString(TcpState s)
{
    switch (s) {
    case TcpState::None:        return QStringLiteral("");
    case TcpState::SynSent:     return QStringLiteral("SYN_SENT");
    case TcpState::SynRecv:     return QStringLiteral("SYN_RECV");
    case TcpState::Established: return QStringLiteral("ESTABLISHED");
    case TcpState::FinWait:     return QStringLiteral("FIN_WAIT");
    case TcpState::CloseWait:   return QStringLiteral("CLOSE_WAIT");
    case TcpState::LastAck:     return QStringLiteral("LAST_ACK");
    case TcpState::TimeWait:    return QStringLiteral("TIME_WAIT");
    case TcpState::Close:       return QStringLiteral("CLOSE");
    case TcpState::SynSent2:    return QStringLiteral("SYN_SENT2");
    }
    return QStringLiteral("?");
}

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

    // Kernel ifindex matching `iface`. 0 = unattributed or unknown.
    // Populated server-side; libqiftop consumers prefer this over `iface`
    // for stable identity (iface names can be reused across netns).
    quint32 ifIndex = 0;

    // Best-effort direction relative to "this host". Populated by the
    // agent (server-side) using inferDirection() before serialising to
    // ConnectionDto, so non-Qt libqiftop consumers don't have to
    // reimplement the heuristic. The client may override with a more
    // local computation if its idea of local addresses / ephemeral range
    // differs from the agent's.
    Direction direction = Direction::Unknown;

    // Conntrack TCP state for TCP flows. `None` for UDP/ICMP/unknown.
    TcpState  tcpState  = TcpState::None;

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
