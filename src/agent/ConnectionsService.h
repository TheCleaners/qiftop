#pragma once

#include <QDBusContext>
#include <QElapsedTimer>
#include <QObject>
#include "dbus/Types.h"

class ConnectionMonitor;

namespace qiftop::backend { class ProcessResolver; }

namespace qiftop::agent {

class IdleManager;

// Implements the org.qiftop.NetworkAgent1.Connections DBus interface on the
// system bus. Wraps the platform ConnectionMonitor (currently
// ConntrackMonitor on Linux) and rebroadcasts each poll snapshot as
// ConnectionsChanged.
class ConnectionsService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiftop.NetworkAgent1.Connections")
    Q_PROPERTY(bool AccountingEnabled READ accountingEnabled NOTIFY AccountingChanged)

public:
    explicit ConnectionsService(ConnectionMonitor *monitor, QObject *parent = nullptr);

    [[nodiscard]] bool accountingEnabled() const { return m_accountingEnabled; }

    void setIdleManager(IdleManager *idle) { m_idle = idle; }

    // Wire in the resolver used to enrich each emitted snapshot with
    // process + container attribution. Optional: when null, attribution
    // fields on the wire stay zero/empty (capability tokens
    // `process-attribution-wire` / `container-attribution-wire` are
    // gated by InterfacesService on the resolver's own capability
    // tokens, so they won't be advertised either).
    //
    // `wantContainerChain` should mirror whether the resolver
    // advertises `container-chain`; passing true with a resolver that
    // doesn't implement the chain just yields single-entry chains
    // (default base-class behaviour wraps resolveContainerForPid).
    void setProcessResolver(backend::ProcessResolver *resolver,
                            bool wantContainerChain = false)
    {
        m_resolver = resolver;
        m_wantContainerChain = wantContainerChain;
    }

public slots:
    dbus::ConnectionDtoList GetConnections();

    // See InterfacesService::SetDesiredIntervalMs for semantics.
    void SetDesiredIntervalMs(uint intervalMs);

    // On-demand process details fetch (capability: on-demand-process-details).
    // Returns a struct with pid=0 when the PID is gone or unreadable —
    // never throws. Cheap (a handful of /proc reads); intended for UI
    // affordances (Copy cmdline, expand process row, etc.) and is NOT
    // called per-tick. Clients should cache by (pid, startTimeJiffies)
    // to survive PID reuse within one boot.
    dbus::ProcessDetailsDto GetProcessDetails(uint pid);

signals:
    // See InterfacesService::StatsChanged for the meaning of `monotonicMs`.
    Q_SCRIPTABLE void ConnectionsChanged(qulonglong monotonicMs,
                                         qiftop::dbus::ConnectionDtoList conns);
    Q_SCRIPTABLE void PermissionDenied(QString detail);
    Q_SCRIPTABLE void AccountingChanged(bool enabled);

private slots:
    void onConnectionsUpdated(const QList<Connection> &conns);
    void onPermissionDenied(const QString &detail);
    void onAccountingUnavailable(const QString &detail);

private:
    ConnectionMonitor       *m_monitor = nullptr;
    IdleManager             *m_idle    = nullptr;
    backend::ProcessResolver*m_resolver= nullptr;
    bool                     m_wantContainerChain = false;
    dbus::ConnectionDtoList  m_last;
    bool                     m_accountingEnabled = true;
    QElapsedTimer            m_clock;       // started in ctor; drives snapshot timestamps
};

} // namespace qiftop::agent
