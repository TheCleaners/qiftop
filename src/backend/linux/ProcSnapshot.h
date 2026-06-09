#pragma once

// Pure helpers for reading a few /proc fields needed to defend against
// PID reuse races. Kept header-only so unit tests can exercise the
// parsing logic with synthetic fixtures (the I/O entry point reads a
// real /proc/<pid>/stat; the parsing entry point takes a QByteArray).

#include <QByteArray>
#include <QFile>

#include <optional>

namespace qiftop::backend::linuximpl::procsnap {

// /proc/<pid>/stat is a single line:
//   pid (comm) state ppid ... starttime ...
// Field 22 (1-based) is starttime in clock ticks since boot.
// The (comm) field can contain spaces and parentheses, so we must
// scan from the LAST ')' rather than splitting naively.
//
// Returns nullopt on any parse failure. Returned value is the raw
// starttime jiffies; only equality matters for our reuse check.
inline std::optional<quint64> parseStartTime(const QByteArray &stat)
{
    const int rp = stat.lastIndexOf(')');
    if (rp < 0 || rp + 2 >= stat.size()) return std::nullopt;
    const QByteArray rest = stat.mid(rp + 2);
    // After ')' there are 21 more fields before starttime (state is
    // index 0 in `rest`, starttime is index 19). Walk by space.
    int pos = 0;
    for (int field = 0; field < 19; ++field) {
        const int sp = rest.indexOf(' ', pos);
        if (sp < 0) return std::nullopt;
        pos = sp + 1;
    }
    const int end = rest.indexOf(' ', pos);
    const QByteArray tok = (end < 0) ? rest.mid(pos)
                                     : rest.mid(pos, end - pos);
    bool ok = false;
    const quint64 v = tok.toULongLong(&ok);
    if (!ok) return std::nullopt;
    return v;
}

// Returns the starttime field of /proc/<pid>/stat, or nullopt if the
// pid does not exist or the file cannot be parsed. Use the returned
// value as an identity token: if a later call returns a DIFFERENT
// value for the same pid, the kernel has reused that pid for a new
// process and any cached attribution is stale.
inline std::optional<quint64> pidStartTime(qint32 pid)
{
    if (pid <= 0) return std::nullopt;
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    return parseStartTime(f.read(4096));
}

} // namespace qiftop::backend::linuximpl::procsnap
