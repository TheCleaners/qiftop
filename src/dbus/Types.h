#pragma once

// DBus-marshallable Data Transfer Objects for the qiftop agent interface.
//
// These are flat structs (no QHostAddress, no enum classes) so QDBusArgument
// operators stay trivial and the wire signature is stable across Qt versions.

#include "backend/Connection.h"
#include "backend/NetworkMonitor.h"

#include <QDBusArgument>
#include <QList>
#include <QString>
#include <QStringList>

namespace qiftop::dbus {

struct InterfaceStatsDto {
    QString     name;
    QString     type;
    quint32     mtu      = 0;
    QStringList addresses;
    quint64     rxBytes   = 0;
    quint64     txBytes   = 0;
    quint64     rxPackets = 0;
    quint64     txPackets = 0;
    bool        isUp        = false;
    bool        isLoopback  = false;
};
using InterfaceStatsDtoList = QList<InterfaceStatsDto>;

// Wire DTO for a single flow. Field order is the DBus tuple signature
// (wire signature: a(yysqysqttttsy) — 13 fields). Append new fields at
// the END; reordering or removing fields requires NetworkAgent2 per
// AGENTS.md §8.
//
// `proto` is encoded as the IANA L4 protocol number (RFC 5237) so non-Qt
// clients (libqiftop consumers, Prometheus exporter, ncurses TUI) can
// decode without knowing the internal L4Proto enum: TCP=6, UDP=17,
// ICMP=1, ICMPv6=58. See backend/Connection.h::toIanaProto.
struct ConnectionDto {
    quint8  proto         = 0;   // IANA L4 protocol number
    quint8  localFamily   = 0;
    QString localAddress;
    quint16 localPort     = 0;
    quint8  remoteFamily  = 0;
    QString remoteAddress;
    quint16 remotePort    = 0;
    quint64 rxBytes       = 0;
    quint64 txBytes       = 0;
    quint64 rxPackets     = 0;
    quint64 txPackets     = 0;
    QString iface;
    quint8  direction     = 0;   // 0=Unknown, 1=Outbound, 2=Inbound — see Direction enum
};
using ConnectionDtoList = QList<ConnectionDto>;

QDBusArgument &operator<<(QDBusArgument &a, const InterfaceStatsDto &s);
const QDBusArgument &operator>>(const QDBusArgument &a, InterfaceStatsDto &s);
QDBusArgument &operator<<(QDBusArgument &a, const ConnectionDto &c);
const QDBusArgument &operator>>(const QDBusArgument &a, ConnectionDto &c);

void registerTypes();

InterfaceStatsDto      toDto(const InterfaceStats &s);
InterfaceStats         fromDto(const InterfaceStatsDto &d);
InterfaceStatsDtoList  toDtos(const QList<InterfaceStats> &list);
QList<InterfaceStats>  fromDtos(const InterfaceStatsDtoList &list);

ConnectionDto          toDto(const Connection &c);
Connection             fromDto(const ConnectionDto &d);
ConnectionDtoList      toDtos(const QList<Connection> &list);
QList<Connection>      fromDtos(const ConnectionDtoList &list);

} // namespace qiftop::dbus

Q_DECLARE_METATYPE(qiftop::dbus::InterfaceStatsDto)
Q_DECLARE_METATYPE(qiftop::dbus::InterfaceStatsDtoList)
Q_DECLARE_METATYPE(qiftop::dbus::ConnectionDto)
Q_DECLARE_METATYPE(qiftop::dbus::ConnectionDtoList)
