#include "Types.h"

#include <QDBusMetaType>
#include <QHostAddress>

namespace qiftop::dbus {

QDBusArgument &operator<<(QDBusArgument &a, const InterfaceStatsDto &s)
{
    a.beginStructure();
    a << s.name << s.type << s.mtu << s.addresses
      << s.rxBytes << s.txBytes << s.rxPackets << s.txPackets
      << s.isUp   << s.isLoopback;
    a.endStructure();
    return a;
}

const QDBusArgument &operator>>(const QDBusArgument &a, InterfaceStatsDto &s)
{
    a.beginStructure();
    a >> s.name >> s.type >> s.mtu >> s.addresses
      >> s.rxBytes >> s.txBytes >> s.rxPackets >> s.txPackets
      >> s.isUp   >> s.isLoopback;
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
      << c.iface;
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
      >> c.iface;
    a.endStructure();
    return a;
}

void registerTypes()
{
    qDBusRegisterMetaType<InterfaceStatsDto>();
    qDBusRegisterMetaType<InterfaceStatsDtoList>();
    qDBusRegisterMetaType<ConnectionDto>();
    qDBusRegisterMetaType<ConnectionDtoList>();
}

InterfaceStatsDto toDto(const InterfaceStats &s)
{
    InterfaceStatsDto d;
    d.name = s.name; d.type = s.type; d.mtu = s.mtu; d.addresses = s.addresses;
    d.rxBytes = s.rxBytes; d.txBytes = s.txBytes;
    d.rxPackets = s.rxPackets; d.txPackets = s.txPackets;
    d.isUp = s.isUp; d.isLoopback = s.isLoopback;
    return d;
}

InterfaceStats fromDto(const InterfaceStatsDto &d)
{
    InterfaceStats s;
    s.name = d.name; s.type = d.type; s.mtu = d.mtu; s.addresses = d.addresses;
    s.rxBytes = d.rxBytes; s.txBytes = d.txBytes;
    s.rxPackets = d.rxPackets; s.txPackets = d.txPackets;
    s.isUp = d.isUp; s.isLoopback = d.isLoopback;
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
    d.proto         = quint8(c.proto);
    d.localFamily   = c.local.isIPv6()  ? 6 : 4;
    d.localAddress  = c.local.address.toString();
    d.localPort     = c.local.port;
    d.remoteFamily  = c.remote.isIPv6() ? 6 : 4;
    d.remoteAddress = c.remote.address.toString();
    d.remotePort    = c.remote.port;
    d.rxBytes = c.rxBytes; d.txBytes = c.txBytes;
    d.rxPackets = c.rxPackets; d.txPackets = c.txPackets;
    d.iface = c.iface;
    return d;
}

Connection fromDto(const ConnectionDto &d)
{
    Connection c;
    c.proto          = static_cast<L4Proto>(d.proto);
    c.local.address  = QHostAddress(d.localAddress);
    c.local.port     = d.localPort;
    c.remote.address = QHostAddress(d.remoteAddress);
    c.remote.port    = d.remotePort;
    c.rxBytes = d.rxBytes; c.txBytes = d.txBytes;
    c.rxPackets = d.rxPackets; c.txPackets = d.txPackets;
    c.iface = d.iface;
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
