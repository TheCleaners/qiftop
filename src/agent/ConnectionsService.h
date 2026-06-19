#pragma once

#include <QDBusContext>
#include <QElapsedTimer>
#include <QObject>
#include "Config.h"
#include "dbus/Types.h"

class ConnectionMonitor;

namespace qiftop::backend { class ProcessResolver; }

namespace qiftop::agent {

class IdleManager;
class AttributionHintManager;

// Implements the org.qiftop.NetworkAgent1.Connections DBus interface on the
// system bus. Wraps the platform ConnectionMonitor (currently
// ConntrackMonitor on Linux) and rebroadcasts each poll snapshot as
// ConnectionsChanged.
class ConnectionsService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiftop.NetworkAgent1.Connections")
    Q_PROPERTY(bool AccountingEnabled READ accountingEnabled NOTIFY AccountingChanged)
    // Current effective attribution eagerness (off/balanced/eager), after
    // applying every live runtime hint over the config default. Read-only;
    // clients drive it via SetDesiredAttributionEagerness. NOTIFY fires the
    // AttributionEagernessChanged signal on every effective-mode change.
    Q_PROPERTY(QString AttributionEagerness READ attributionEagerness
               NOTIFY AttributionEagernessChanged)

public:
    explicit ConnectionsService(ConnectionMonitor *monitor, QObject *parent = nullptr);

    [[nodiscard]] bool accountingEnabled() const { return m_accountingEnabled; }

    // Effective attribution eagerness as a lowercase string. Falls back to
    // "balanced" when no hint manager is wired (e.g. bare unit tests).
    [[nodiscard]] QString attributionEagerness() const;

    void setIdleManager(IdleManager *idle) { m_idle = idle; }

    // Wire in the attribution eagerness hint manager. When set, the
    // SetDesiredAttributionEagerness method records hints here, the
    // AttributionEagerness property reflects its effective mode, and every
    // effective-mode change is re-tuned into the wired resolver and mirrored
    // via the AttributionEagernessChanged signal (see onEffectiveModeChanged).
    void setAttributionHintManager(AttributionHintManager *mgr);

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

    // Install the disclosure policy for GetProcessDetails' privileged fields
    // (exe/cwd/cmdline). Defaults to Owner (root or the PID owner) when never
    // set — the safe, documented default.
    void setProcessDetailsPolicy(const ProcessDetailsPolicy &policy)
    {
        m_detailsPolicy = policy;
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

    // See SetDesiredAttributionEagerness doc on the declaration below.
    QString SetDesiredAttributionEagerness(const QString &mode);

signals:
    // See InterfacesService::StatsChanged for the meaning of `monotonicMs`.
    Q_SCRIPTABLE void ConnectionsChanged(qulonglong monotonicMs,
                                         qiftop::dbus::ConnectionDtoList conns);
    Q_SCRIPTABLE void PermissionDenied(QString detail);
    Q_SCRIPTABLE void AccountingChanged(bool enabled);
    // Fires whenever the effective attribution eagerness changes (a hint was
    // set/cleared, a hint expired, or a hinting peer disconnected). Carries
    // the new effective mode string (off/balanced/eager).
    Q_SCRIPTABLE void AttributionEagernessChanged(QString mode);

private slots:
    void onConnectionsUpdated(const QList<Connection> &conns);
    void onPermissionDenied(const QString &detail);
    void onAccountingUnavailable(const QString &detail);
    // Mirror the hint manager's effective-mode transition onto the wire:
    // emit AttributionEagernessChanged + re-tune the wired resolver.
    void onEffectiveEagernessChanged(qiftop::backend::AttributionEagerness mode);

private:
    // True if the D-Bus caller may see a process's privileged detail fields
    // (exe/cwd/cmdline): caller is root or owns the target PID. Fails safe
    // (false) when the caller uid can't be established. In-process callers
    // (no D-Bus context) are the agent itself and always allowed.
    [[nodiscard]] bool callerMaySeeProcessFields(quint32 targetUid) const;

    ConnectionMonitor       *m_monitor = nullptr;
    IdleManager             *m_idle    = nullptr;
    AttributionHintManager  *m_attrHints = nullptr;
    backend::ProcessResolver*m_resolver= nullptr;
    bool                     m_wantContainerChain = false;
    ProcessDetailsPolicy     m_detailsPolicy;   // default: Owner
    dbus::ConnectionDtoList  m_last;
    bool                     m_accountingEnabled = true;
    QElapsedTimer            m_clock;       // started in ctor; drives snapshot timestamps
};

} // namespace qiftop::agent
