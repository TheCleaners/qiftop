#pragma once

#include <QObject>
#include <QStringList>

#include "Connection.h"
#include "ProcessDetails.h"

// Abstract base for per-connection traffic monitors.
//
// Implementations live in backend/<platform>/ and run their capture loop
// in a worker thread, emitting connectionsUpdated via a queued connection.
//
// Each update should contain the full current set of active flows; the model
// is responsible for diffing/expiring.
class ConnectionMonitor : public QObject {
    Q_OBJECT

public:
    explicit ConnectionMonitor(QObject *parent = nullptr);
    ~ConnectionMonitor() override;

    virtual void start() = 0;
    virtual void stop()  = 0;
    virtual void setPollIntervalMs(int /*ms*/) {}
    virtual void setDesiredIntervalMs(int /*ms*/) {}

    // Lazily fetch extended details (exe / cmdline / cwd / start time)
    // for a pid. Deliberately NOT carried in every snapshot — see the
    // "default-cheap pipeline" principle. The result arrives
    // asynchronously via processDetailsReady(). Default: no-op (backends
    // without the capability — in-process, or an old agent — simply
    // never answer, and the UI falls back to the wire fields). The DBus
    // client backend overrides this to call the agent's
    // Connections.GetProcessDetails(pid) RPC.
    virtual void requestProcessDetails(qint32 /*pid*/) {}

    // Capability tokens this backend's data path actually provides, in the
    // SAME vocabulary as the agent's DBus `Capabilities` property
    // (AGENTS.md §4). Transport-neutral: the DBus proxy lets the merged
    // agent list ride on the Interfaces monitor and returns empty here; an
    // in-process backend reports the per-flow tokens it genuinely fills
    // (e.g. iana-proto / tcp-state, plus *-attribution-wire only when a
    // resolver is actually wired). The client unions this with the
    // NetworkMonitor's list. Only advertise tokens for data you deliver.
    [[nodiscard]] virtual QStringList capabilities() const { return {}; }

signals:
    void connectionsUpdated(QList<Connection> connections);
    void permissionDenied(QString detail);

    // Emitted (once) when the backend determines that the kernel is not
    // recording per-flow byte/packet counters (e.g. Linux's
    // `net.netfilter.nf_conntrack_acct` sysctl is off and we couldn't turn
    // it on). The UI surfaces this as a non-fatal hint.
    void accountingUnavailable(QString detail);

    // Async reply to requestProcessDetails(). `details.pid == 0` means
    // the process was not reachable (exited / unknown).
    void processDetailsReady(qiftop::backend::ProcessDetails details);
};
