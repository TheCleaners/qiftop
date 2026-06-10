#pragma once

#include <QString>

namespace qiftop::backend {

// On-demand process detail, fetched lazily (not carried in every
// snapshot — see the "default-cheap pipeline" principle). The agent's
// Connections.GetProcessDetails(pid) RPC produces these; the
// DBusConnectionMonitor maps the wire DTO into this backend value type
// so the abstract ConnectionMonitor interface (and the UI) never need
// to include dbus/Types.h.
//
// pid == 0 is the sentinel "process not reachable / unknown" reply.
struct ProcessDetails {
    qint32  pid = 0;
    quint32 uid = 0;
    QString comm;
    QString exe;
    QString cmdline;
    QString cwd;
    quint64 startTimeJiffies = 0;

    [[nodiscard]] bool valid() const { return pid > 0; }

    friend bool operator==(const ProcessDetails &, const ProcessDetails &) = default;
};

} // namespace qiftop::backend

Q_DECLARE_METATYPE(qiftop::backend::ProcessDetails)
