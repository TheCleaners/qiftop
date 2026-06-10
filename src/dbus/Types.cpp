#include "Types.h"

#include <QDBusMetaType>
#include <QHostAddress>

#include <algorithm>
#include <limits>

namespace qiftop::dbus {

// Receive-side bound on containerChain. Mirrors the server-side
// kMaxContainerChainDepth in the cgroup classifier: a malicious or buggy
// agent could ship an arbitrarily long chain and DoS the client's
// memory/CPU. Truncation happens AFTER unmarshalling, so the wire
// signature is unchanged.
constexpr qsizetype kMaxContainerChainDepth = 16;

QDBusArgument &operator<<(QDBusArgument &a, const InterfaceStatsDto &s)
{
    a.beginStructure();
    a << s.name << s.type << s.mtu << s.addresses
      << s.rxBytes << s.txBytes << s.rxPackets << s.txPackets
      << s.isUp   << s.isLoopback
      << s.ifIndex << s.operState
      << s.rxErrors << s.txErrors << s.rxDropped << s.txDropped;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, InterfaceStatsDto &s)
{
    a.beginStructure();
    a >> s.name >> s.type >> s.mtu >> s.addresses
      >> s.rxBytes >> s.txBytes >> s.rxPackets >> s.txPackets
      >> s.isUp   >> s.isLoopback
      >> s.ifIndex >> s.operState
      >> s.rxErrors >> s.txErrors >> s.rxDropped >> s.txDropped;
    a.endStructure();
    return a;
}

QDBusArgument &operator<<(QDBusArgument &a, const ContainerInfoDto &c)
{
    a.beginStructure();
    a << c.runtime << c.id << c.name;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, ContainerInfoDto &c)
{
    a.beginStructure();
    a >> c.runtime >> c.id >> c.name;
    a.endStructure();
    return a;
}

QDBusArgument &operator<<(QDBusArgument &a, const ConnectionDto &c)
{
    a.beginStructure();
    a << c.proto
      << c.localFamily  << c.localAddress  << c.localPort
      << c.remoteFamily << c.remoteAddress << c.remotePort
      << c.rxBytes << c.txBytes << c.rxPackets << c.txPackets
      << c.iface
      << c.direction
      << c.ifIndex << c.tcpState
      // v0.4 attribution (append-only)
      << c.pid << c.uid << c.comm
      << c.containerRuntime << c.containerId << c.containerName
      << c.containerChain;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, ConnectionDto &c)
{
    a.beginStructure();
    a >> c.proto
      >> c.localFamily  >> c.localAddress  >> c.localPort
      >> c.remoteFamily >> c.remoteAddress >> c.remotePort
      >> c.rxBytes >> c.txBytes >> c.rxPackets >> c.txPackets
      >> c.iface
      >> c.direction
      >> c.ifIndex >> c.tcpState
      >> c.pid >> c.uid >> c.comm
      >> c.containerRuntime >> c.containerId >> c.containerName
      >> c.containerChain;
    // Clamp wire-sourced chain length; an over-long chain from a
    // malicious/buggy agent must not be retained on the receiver.
    if (c.containerChain.size() > kMaxContainerChainDepth)
        c.containerChain.resize(kMaxContainerChainDepth);
    a.endStructure();
    return a;
}

void registerTypes()
{
    qDBusRegisterMetaType<InterfaceStatsDto>();
    qDBusRegisterMetaType<InterfaceStatsDtoList>();
    qDBusRegisterMetaType<ContainerInfoDto>();
    qDBusRegisterMetaType<ContainerInfoDtoList>();
    qDBusRegisterMetaType<ConnectionDto>();
    qDBusRegisterMetaType<ConnectionDtoList>();
    qDBusRegisterMetaType<ProcessDetailsDto>();
}

QDBusArgument &operator<<(QDBusArgument &a, const ProcessDetailsDto &p)
{
    a.beginStructure();
    a << p.pid << p.uid << p.comm << p.exe << p.cmdline << p.cwd
      << p.startTimeJiffies;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, ProcessDetailsDto &p)
{
    a.beginStructure();
    a >> p.pid >> p.uid >> p.comm >> p.exe >> p.cmdline >> p.cwd
      >> p.startTimeJiffies;
    a.endStructure();
    return a;
}

InterfaceStatsDto toDto(const InterfaceStats &s)
{
    InterfaceStatsDto d;
    d.name = s.name; d.type = s.type; d.mtu = s.mtu; d.addresses = s.addresses;
    d.rxBytes = s.rxBytes; d.txBytes = s.txBytes;
    d.rxPackets = s.rxPackets; d.txPackets = s.txPackets;
    d.isUp = s.isUp; d.isLoopback = s.isLoopback;
    d.ifIndex = s.ifIndex; d.operState = s.operState;
    d.rxErrors = s.rxErrors; d.txErrors = s.txErrors;
    d.rxDropped = s.rxDropped; d.txDropped = s.txDropped;
    return d;
}

InterfaceStats fromDto(const InterfaceStatsDto &d)
{
    InterfaceStats s;
    s.name = d.name; s.type = d.type; s.mtu = d.mtu; s.addresses = d.addresses;
    s.rxBytes = d.rxBytes; s.txBytes = d.txBytes;
    s.rxPackets = d.rxPackets; s.txPackets = d.txPackets;
    s.isUp = d.isUp; s.isLoopback = d.isLoopback;
    s.ifIndex = d.ifIndex; s.operState = d.operState;
    s.rxErrors = d.rxErrors; s.txErrors = d.txErrors;
    s.rxDropped = d.rxDropped; s.txDropped = d.txDropped;
    return s;
}

InterfaceStatsDtoList toDtos(const QList<InterfaceStats> &list)
{
    InterfaceStatsDtoList out; out.reserve(list.size());
    for (const auto &s : list) out << toDto(s);
    return out;
}

QList<InterfaceStats> fromDtos(const InterfaceStatsDtoList &list)
{
    QList<InterfaceStats> out; out.reserve(list.size());
    for (const auto &d : list) out << fromDto(d);
    return out;
}

ConnectionDto toDto(const Connection &c)
{
    ConnectionDto d;
    d.proto         = toIanaProto(c.proto);
    d.localFamily   = c.local.isIPv6()  ? 6 : 4;
    d.localAddress  = c.local.address.toString();
    d.localPort     = c.local.port;
    d.remoteFamily  = c.remote.isIPv6() ? 6 : 4;
    d.remoteAddress = c.remote.address.toString();
    d.remotePort    = c.remote.port;
    d.rxBytes = c.rxBytes; d.txBytes = c.txBytes;
    d.rxPackets = c.rxPackets; d.txPackets = c.txPackets;
    d.iface = c.iface;
    d.direction = quint8(c.direction);
    d.ifIndex   = c.ifIndex;
    d.tcpState  = quint8(c.tcpState);
    // v0.4 attribution — bulk fields only (exe/cmdline/cwd are on-demand).
    d.pid  = c.process.pid > 0 ? quint32(c.process.pid) : 0;
    d.uid  = c.process.uid;
    d.comm = c.process.comm;
    d.containerRuntime = c.container.runtime;
    d.containerId      = c.container.id;
    d.containerName    = c.container.name;
    d.containerChain.reserve(c.containerChain.size());
    for (const auto &ci : c.containerChain) {
        d.containerChain << ContainerInfoDto{ci.runtime, ci.id, ci.name};
    }
    return d;
}

Connection fromDto(const ConnectionDto &d)
{
    Connection c;
    c.proto          = fromIanaProto(d.proto);
    c.local.address  = QHostAddress(d.localAddress);
    c.local.port     = d.localPort;
    c.remote.address = QHostAddress(d.remoteAddress);
    c.remote.port    = d.remotePort;
    c.rxBytes = d.rxBytes; c.txBytes = d.txBytes;
    c.rxPackets = d.rxPackets; c.txPackets = d.txPackets;
    c.iface = d.iface;
    // Clamp out-of-range direction values to Unknown rather than UB.
    c.direction = (d.direction <= quint8(Direction::Inbound))
                  ? static_cast<Direction>(d.direction)
                  : Direction::Unknown;
    c.ifIndex   = d.ifIndex;
    // Clamp TCP state; unknown values become None rather than UB.
    c.tcpState  = (d.tcpState <= quint8(TcpState::SynSent2))
                  ? static_cast<TcpState>(d.tcpState)
                  : TcpState::None;
    // v0.4 attribution. pid is qint32 in ProcessInfo; clamp INT_MAX to be safe.
    c.process.pid  = (d.pid <= quint32(std::numeric_limits<qint32>::max()))
                     ? qint32(d.pid) : 0;
    c.process.uid  = d.uid;
    c.process.comm = d.comm;
    c.container.runtime = d.containerRuntime;
    c.container.id      = d.containerId;
    c.container.name    = d.containerName;
    // Clamp chain depth on the receive path (matches the server-side
    // classifier cap) so a hostile DTO can't balloon client memory.
    const qsizetype chainLen =
        std::min(d.containerChain.size(), kMaxContainerChainDepth);
    c.containerChain.reserve(chainLen);
    for (qsizetype i = 0; i < chainLen; ++i) {
        const auto &ci = d.containerChain.at(i);
        c.containerChain << qiftop::backend::ContainerInfo{ci.runtime, ci.id, ci.name};
    }
    return c;
}

ConnectionDtoList toDtos(const QList<Connection> &list)
{
    ConnectionDtoList out; out.reserve(list.size());
    for (const auto &c : list) out << toDto(c);
    return out;
}

QList<Connection> fromDtos(const ConnectionDtoList &list)
{
    QList<Connection> out; out.reserve(list.size());
    for (const auto &d : list) out << fromDto(d);
    return out;
}

} // namespace qiftop::dbus
