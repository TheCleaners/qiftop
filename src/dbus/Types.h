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
    bool        isUp        = false;     // IFF_UP (admin flag) — kept for back-compat
    bool        isLoopback  = false;
    // v0.3 additions (capability tokens: ifindex, oper-state, link-errors).
    // Old (v0.1/0.2) clients unmarshalling this struct will fail at the
    // ifIndex field; agents detect that path via the version probe and
    // clients fall back to the in-process backend.
    quint32     ifIndex     = 0;
    quint8      operState   = 0;         // IF_OPER_* (RFC 2863); 0 = unknown.
    quint64     rxErrors    = 0;
    quint64     txErrors    = 0;
    quint64     rxDropped   = 0;
    quint64     txDropped   = 0;
};
using InterfaceStatsDtoList = QList<InterfaceStatsDto>;

// Container / cgroup-scope metadata that may be attached to a Connection.
//
// `runtime` is the lowercase runtime name (`"docker"`, `"containerd"`,
// `"podman"`, `"systemd"`, `"lxc"`, `"nspawn"`, ...). `id` is the
// displayable identifier (12-char hex prefix for content-addressable
// runtimes; full human name for name-ID runtimes like nspawn/lxd/lxc;
// see docs/ATTRIBUTION.md §5c).
struct ContainerInfoDto {
    QString runtime;
    QString id;
    QString name;

    friend bool operator==(const ContainerInfoDto &, const ContainerInfoDto &) = default;
};
using ContainerInfoDtoList = QList<ContainerInfoDto>;

// Wire DTO for a single flow. Field order is the DBus tuple signature.
// Wire signature is (yysqysqttttsyuy uussss a(sss)) — 22 outer fields
// plus a nested struct array. Append new fields at the END; reordering
// or removing fields requires NetworkAgent2 per AGENTS.md §8.
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
    // v0.3 additions.
    quint32 ifIndex       = 0;   // Matches `iface`; 0 = unknown.
    quint8  tcpState      = 0;   // TCP_CONNTRACK_*; 0 (None) for non-TCP — see TcpState enum
    // v0.4 additions — process + container attribution (bulk fields only).
    // Populated server-side when the matching capability token is
    // advertised: process-attribution-wire, container-attribution-wire,
    // container-chain-wire. Empty / zero when the resolver returned no
    // useful info or no resolver is wired.
    //
    // Expensive enrichment (exe path, cmdline, cwd) is NOT shipped on
    // the wire — fetched on demand via GetProcessDetails(pid) per the
    // "default-cheap pipeline" design principle (docs/ATTRIBUTION.md
    // §7, planned).
    quint32 pid           = 0;   // 0 = unknown/unattributed
    quint32 uid           = 0;   // process uid (0 is meaningful only when pid != 0)
    QString comm;                // basename, kernel-truncated to 15 bytes
    QString containerRuntime;    // empty when no container
    QString containerId;
    QString containerName;
    // Outer → inner. Empty when container-chain-wire capability absent
    // OR the flow has no container ancestry. Leaf entry equals
    // (containerRuntime, containerId, containerName) when both populated.
    ContainerInfoDtoList containerChain;
};
using ConnectionDtoList = QList<ConnectionDto>;

// On-demand process details (v0.4): the expensive enrichment that is
// deliberately NOT shipped in the bulk ConnectionsChanged signal.
// Clients fetch this per-PID via GetProcessDetails(pid) when the user
// expands a row or opens a context-menu action. Cache key on the
// client side is (pid, startTimeJiffies) — startTime monotonically
// distinguishes PID reuse on a single boot.
struct ProcessDetailsDto {
    quint32 pid               = 0;   // 0 ⇒ process not reachable
    quint32 uid               = 0;
    QString comm;
    QString exe;
    QString cmdline;
    QString cwd;
    quint64 startTimeJiffies  = 0;

    friend bool operator==(const ProcessDetailsDto &, const ProcessDetailsDto &) = default;
};

QDBusArgument &operator<<(QDBusArgument &a, const InterfaceStatsDto &s);
const QDBusArgument &operator>>(const QDBusArgument &a, InterfaceStatsDto &s);
QDBusArgument &operator<<(QDBusArgument &a, const ContainerInfoDto &c);
const QDBusArgument &operator>>(const QDBusArgument &a, ContainerInfoDto &c);
QDBusArgument &operator<<(QDBusArgument &a, const ConnectionDto &c);
const QDBusArgument &operator>>(const QDBusArgument &a, ConnectionDto &c);
QDBusArgument &operator<<(QDBusArgument &a, const ProcessDetailsDto &p);
const QDBusArgument &operator>>(const QDBusArgument &a, ProcessDetailsDto &p);

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
Q_DECLARE_METATYPE(qiftop::dbus::ContainerInfoDto)
Q_DECLARE_METATYPE(qiftop::dbus::ContainerInfoDtoList)
Q_DECLARE_METATYPE(qiftop::dbus::ConnectionDto)
Q_DECLARE_METATYPE(qiftop::dbus::ConnectionDtoList)
Q_DECLARE_METATYPE(qiftop::dbus::ProcessDetailsDto)
