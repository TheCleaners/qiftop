#pragma once

#include <QDBusContext>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include "dbus/Types.h"

class NetworkMonitor;

namespace qiftop::backend { class ProcessResolver; }

namespace qiftop::agent {

class IdleManager;

// Implements the org.qiftop.NetworkAgent1.Interfaces DBus interface on the
// system bus. Wraps the platform NetworkMonitor (currently NetlinkMonitor on
// Linux) and rebroadcasts its periodic snapshots as a `StatsChanged` signal.
//
// Also serves as the contract-version surface for the agent as a whole:
// the `Version` and `Capabilities` properties let clients tell which
// agent they're talking to without parsing introspection XML or
// guessing from method-call AccessDenied errors. Adding a new optional
// feature: bump capability tokens here, leave Version alone for non-
// breaking additions; breaking changes still require a NetworkAgent2
// interface per AGENTS.md §8.
class InterfacesService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.qiftop.NetworkAgent1.Interfaces")
    Q_PROPERTY(QString     Version      READ version      CONSTANT)
    Q_PROPERTY(QStringList Capabilities READ capabilities CONSTANT)

public:
    explicit InterfacesService(NetworkMonitor *monitor, QObject *parent = nullptr);

    void setIdleManager(IdleManager *idle);
    // Optional: a ProcessResolver whose runtime-detected capability tokens
    // are merged into the agent's Capabilities property. Pass null (the
    // default) to advertise only the base agent capabilities — used when
    // attribution features are compiled out or the runtime probe failed.
    void setProcessResolver(backend::ProcessResolver *resolver);

    [[nodiscard]] QString     version()      const;
    [[nodiscard]] QStringList capabilities() const;

public slots:
    // DBus method: returns the most recent per-interface snapshot. Useful
    // for late-joining clients that don't want to wait for the next tick.
    dbus::InterfaceStatsDtoList GetInterfaces();

    // DBus method: clients request a desired polling cadence in ms.
    // The agent honours min() across all active hints (clamped to the
    // configured floor). Hints expire after Config::hintTtlMs unless
    // re-asserted. Pass 0 (or omit) to clear this client's hint.
    void SetDesiredIntervalMs(uint intervalMs);

signals:
    // DBus signal — fires on each polling tick of the backend.
    //
    // `monotonicMs` is a CLOCK_MONOTONIC-based millisecond counter (Qt's
    // `QElapsedTimer::msecsSinceReference()` clock) sampled at the moment
    // the agent emits this snapshot. Consumers building rate series
    // (Prometheus exporter, alerting, libqiftop history) should prefer
    // this to local arrival time so DBus delivery jitter and small CPU
    // hiccups don't corrupt their Δt. It is NOT a wall-clock and is not
    // comparable across `qiftop-agent` restarts.
    Q_SCRIPTABLE void StatsChanged(qulonglong monotonicMs,
                                   qiftop::dbus::InterfaceStatsDtoList stats);

    // DBus signal — mirrors IdleManager::cadenceChanged. Fires whenever
    // the effective polling interval changes (sped up, slowed down, or
    // paused). `ms == 0` means paused. Clients use this to notice their
    // own SetDesiredIntervalMs heartbeat slipping past hintTtlMs (e.g.
    // after a UI hang) before inferring from missing StatsChanged.
    Q_SCRIPTABLE void CadenceChanged(uint intervalMs);

private slots:
    void onStatsUpdated(const QList<InterfaceStats> &stats);
    void onCadenceChanged(int ms);

private:
    NetworkMonitor              *m_monitor  = nullptr;
    IdleManager                 *m_idle     = nullptr;
    backend::ProcessResolver    *m_resolver = nullptr;
    dbus::InterfaceStatsDtoList  m_last;
    QElapsedTimer                m_clock;        // started in ctor; drives snapshot timestamps
};

} // namespace qiftop::agent
