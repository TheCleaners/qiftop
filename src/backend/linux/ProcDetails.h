#pragma once

// On-demand process details reader: a lightweight /proc snapshot used
// by the agent's GetProcessDetails RPC. Pulled out of any monitor so
// it can be tested with a fixture directory (procRoot parameter) and
// shared between the agent and any future libqiftop consumer.
//
// Contract: each field is best-effort. If /proc/<pid>/cmdline is empty
// (kernel thread), cmdline stays empty. If exe/cwd symlinks can't be
// read (process exited, EPERM in unprivileged contexts), those stay
// empty. Only `valid` indicates whether the PID was reachable at all.

#include <QString>

namespace qiftop::backend::linux_ {

struct ProcessDetails {
    qint32  pid               = 0;   // mirrors input on success; 0 on miss
    quint32 uid               = 0;
    QString comm;                    // basename, kernel-truncated to 15 bytes
    QString exe;                     // resolved /proc/<pid>/exe symlink
    QString cmdline;                 // NULs → spaces, trimmed
    QString cwd;                     // resolved /proc/<pid>/cwd symlink
    quint64 startTimeJiffies = 0;    // /proc/<pid>/stat field 22 — for (pid, starttime) cache keys
    bool    valid            = false;
};

// Read /proc/<pid>/{status, cmdline, exe, cwd, stat}. Returns a struct
// with valid=false (and pid=0) when /proc/<pid>/status can't be opened
// at all — typically because the PID is gone or we lack permission.
// PID-reuse safe: starttime is snapshotted before the reads and
// re-checked after; a mismatch (pid recycled mid-read) also yields
// valid=false rather than fields mixed from two processes.
//
// procRoot defaults to "/proc" in production; tests pass a fixture
// directory rooted at a tempdir.
[[nodiscard]] ProcessDetails readProcessDetails(qint32 pid,
                                                const QString &procRoot = QStringLiteral("/proc"));

} // namespace qiftop::backend::linux_
