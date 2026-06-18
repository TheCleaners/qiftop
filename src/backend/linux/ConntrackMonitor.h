#pragma once

#include <QStringList>
#include <QThread>

#include <memory>

#include "backend/ConnectionMonitor.h"

namespace qiftop::backend { class ProcessResolver; }

// Linux ConnectionMonitor — libnetfilter_conntrack implementation.
//
// Owns a QThread; the worker polls conntrack on a tick and emits
// connectionsUpdated with the current flow set + per-flow byte/packet
// counters. Issues separate AF_INET and AF_INET6 dumps because the
// AF_UNSPEC dump path is unreliable across kernel/libnetfilter_conntrack
// combos and often returns only IPv4 entries.
//
// Process/container attribution: the constructor builds the default
// ProcessResolver (sock_diag + cgroup + netns layers per the build) on the
// main thread and hands it to the worker, which runs attributeFlows() over
// each snapshot before emitting — exactly mirroring the agent path, just
// in-process for the self-elevated / no-agent fallback. Unprivileged runs
// attribute only the user's own processes; that's honest and reflected in
// the resolver's probed capabilities().
class ConntrackMonitor : public ConnectionMonitor {
    Q_OBJECT

public:
    explicit ConntrackMonitor(QObject *parent = nullptr);

    // Test seam: inject a resolver (e.g. a fake advertising chosen
    // capabilities) instead of building the platform default. Used by
    // test_backend_capabilities to pin the resolver-caps → *-attribution-wire
    // token mapping without needing root or a live sock_diag socket.
    explicit ConntrackMonitor(std::unique_ptr<qiftop::backend::ProcessResolver> resolver,
                              QObject *parent = nullptr);

    ~ConntrackMonitor() override;

    void start() override;
    void stop()  override;
    void setPollIntervalMs(int ms) override;

    // Structural tokens the conntrack dump genuinely delivers (iana-proto,
    // tcp-state) PLUS the *-attribution-wire tokens derived from the wired
    // resolver's probed capabilities (process/container/chain). We do NOT
    // advertise direction-on-wire or attribution-reason: those are inferred
    // client-side, not computed here.
    [[nodiscard]] QStringList capabilities() const override;

private:
    class Worker;

    // Shared construction body: cache resolver caps (computed once on the
    // main thread, so reads from capabilities() are thread-safe), hand the
    // resolver to the worker, and wire the thread + signals.
    void init(std::unique_ptr<qiftop::backend::ProcessResolver> resolver);

    QThread     m_thread;
    Worker     *m_worker = nullptr; // owned by m_thread
    QStringList m_resolverCaps;     // resolver->capabilities(), snapshotted on construct
};
