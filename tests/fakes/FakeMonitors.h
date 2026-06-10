// Reusable in-memory fakes for the abstract backend monitors + DNS
// resolver. Lets widget-level / agent-level tests instantiate the
// real production wiring (MainWindow, ConnectionsService, …) without
// requiring a live kernel, libnl, conntrack, or DBus connection.

#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include "backend/Connection.h"
#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"
#include "dns/DnsResolver.h"

namespace qiftop::tests {

class FakeNetworkMonitor final : public NetworkMonitor {
    Q_OBJECT
public:
    using NetworkMonitor::NetworkMonitor;

    void start() override            { ++startCalls; }
    void stop()  override            { ++stopCalls; }
    void setPollIntervalMs(int ms) override
    {
        ++setPollCalls;
        lastPollMs = ms;
    }
    void setDesiredIntervalMs(int ms) override
    {
        ++setDesiredCalls;
        lastDesiredMs = ms;
    }

    void emitSnapshot(QList<InterfaceStats> stats)
    {
        emit statsUpdated(std::move(stats));
    }

    int startCalls       = 0;
    int stopCalls        = 0;
    int setPollCalls     = 0;
    int setDesiredCalls  = 0;
    int lastPollMs       = -1;
    int lastDesiredMs    = -1;
};

class FakeConnectionMonitor final : public ConnectionMonitor {
    Q_OBJECT
public:
    using ConnectionMonitor::ConnectionMonitor;

    void start() override            { ++startCalls; }
    void stop()  override            { ++stopCalls; }
    void setPollIntervalMs(int ms) override
    {
        ++setPollCalls;
        lastPollMs = ms;
    }
    void setDesiredIntervalMs(int ms) override
    {
        ++setDesiredCalls;
        lastDesiredMs = ms;
    }

    void emitSnapshot(QList<Connection> conns)
    {
        emit connectionsUpdated(std::move(conns));
    }
    void emitPermissionDenied(const QString &detail)
    {
        emit permissionDenied(detail);
    }
    void emitAccountingUnavailable(const QString &detail)
    {
        emit accountingUnavailable(detail);
    }

    int startCalls       = 0;
    int stopCalls        = 0;
    int setPollCalls     = 0;
    int setDesiredCalls  = 0;
    int lastPollMs       = -1;
    int lastDesiredMs    = -1;
};

// DNS resolver fake with a programmable cache. Tests stage answers
// via setCached() (synchronous lookups through cachedName) and/or
// emitResolved() (asynchronous answer arriving after the row was
// first painted — the bug-process-empty failure shape).
class FakeDnsResolver final : public DnsResolver {
    Q_OBJECT
public:
    using DnsResolver::DnsResolver;

    [[nodiscard]] QString cachedName(const QHostAddress &addr) const override
    {
        return m_cache.value(addr.toString());
    }
    void resolve(const QHostAddress &)  override { ++resolveCalls; }
    void clearCache() override
    {
        ++clearCalls;
        m_cache.clear();
    }

    void setCached(const QHostAddress &addr, const QString &name)
    {
        m_cache.insert(addr.toString(), name);
    }
    void emitResolved(const QHostAddress &addr, const QString &name)
    {
        emit resolved(addr, name);
    }

    int resolveCalls = 0;
    int clearCalls   = 0;

private:
    QHash<QString, QString> m_cache;  // keyed by addr.toString()
};

// Convenience builders so scenario tests stay readable.

inline InterfaceStats mkIface(const char *name, quint32 ifIndex,
                              quint64 rxBytes = 0, quint64 txBytes = 0)
{
    InterfaceStats s;
    s.name      = QString::fromLatin1(name);
    s.type      = QStringLiteral("ethernet");
    s.ifIndex   = ifIndex;
    s.operState = 6;  // UP
    s.isUp      = true;
    s.rxBytes   = rxBytes;
    s.txBytes   = txBytes;
    return s;
}

inline Connection mkFlow(const char *iface,
                         const char *localAddr,  quint16 localPort,
                         const char *remoteAddr, quint16 remotePort,
                         L4Proto proto = L4Proto::Tcp,
                         quint64 rxBytes = 0, quint64 txBytes = 0,
                         Direction direction = Direction::Unknown)
{
    Connection c;
    c.proto         = proto;
    c.iface         = QString::fromLatin1(iface);
    c.local.address = QHostAddress(QString::fromLatin1(localAddr));
    c.local.port    = localPort;
    c.remote.address = QHostAddress(QString::fromLatin1(remoteAddr));
    c.remote.port    = remotePort;
    c.rxBytes       = rxBytes;
    c.txBytes       = txBytes;
    // Direction::Unknown lets ConnectionModel run its own inference
    // (host-dependent ephemeral ranges); pass an explicit value when
    // the test needs deterministic direction regardless of host config.
    c.direction     = direction;
    return c;
}

} // namespace qiftop::tests
