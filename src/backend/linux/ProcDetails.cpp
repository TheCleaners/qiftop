#include "ProcDetails.h"

#include <QFile>
#include <QFileInfo>

namespace qiftop::backend::linux_ {

namespace {

QString readStatusComm(const QString &procRoot, qint32 pid, quint32 &uid)
{
    QFile f(QStringLiteral("%1/%2/status").arg(procRoot).arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = f.read(8192);
    QString comm;
    for (const auto &line : data.split('\n')) {
        if (line.startsWith("Name:")) {
            comm = QString::fromUtf8(line.mid(5).trimmed());
        } else if (line.startsWith("Uid:")) {
            const auto parts = QByteArray(line.mid(4)).simplified().split(' ');
            if (!parts.isEmpty()) uid = parts.first().toUInt();
        }
    }
    return comm;
}

QString readCmdline(const QString &procRoot, qint32 pid)
{
    QFile f(QStringLiteral("%1/%2/cmdline").arg(procRoot).arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray data = f.read(8192);
    // NUL-separated argv → space-joined for display.
    for (auto &b : data) if (b == '\0') b = ' ';
    return QString::fromUtf8(data).trimmed();
}

QString readSymlink(const QString &procRoot, qint32 pid, const char *name)
{
    return QFileInfo(QStringLiteral("%1/%2/%3").arg(procRoot)
                         .arg(pid).arg(QLatin1String(name))).symLinkTarget();
}

// /proc/<pid>/stat field 22 = starttime in jiffies since boot.
// The format is `pid (comm) state ppid ...` and (comm) may contain
// arbitrary characters including parens + spaces, so we have to find
// the LAST ')' and tokenise everything after it. Field 22 is then
// index 19 in the post-`)` token stream (state is 0, ppid 1, ...,
// starttime 19).
quint64 readStartTimeJiffies(const QString &procRoot, qint32 pid)
{
    QFile f(QStringLiteral("%1/%2/stat").arg(procRoot).arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray data = f.read(4096);
    const int rparen = data.lastIndexOf(')');
    if (rparen < 0 || rparen + 2 >= data.size()) return 0;
    const auto rest = QByteArray(data.mid(rparen + 2)).simplified().split(' ');
    constexpr int kStartTimeFieldOffset = 19; // 22nd field, 0-indexed after rparen
    if (rest.size() <= kStartTimeFieldOffset) return 0;
    return rest[kStartTimeFieldOffset].toULongLong();
}

} // namespace

ProcessDetails readProcessDetails(qint32 pid, const QString &procRoot)
{
    ProcessDetails d;
    if (pid <= 0) return d;

    quint32 uid = 0;
    const QString comm = readStatusComm(procRoot, pid, uid);
    if (comm.isEmpty()) {
        // /proc/<pid>/status unreadable → PID gone or EACCES. Caller
        // sees valid=false, pid=0.
        return d;
    }
    d.pid              = pid;
    d.uid              = uid;
    d.comm             = comm;
    d.cmdline          = readCmdline(procRoot, pid);
    d.exe              = readSymlink(procRoot, pid, "exe");
    d.cwd              = readSymlink(procRoot, pid, "cwd");
    d.startTimeJiffies = readStartTimeJiffies(procRoot, pid);
    d.valid            = true;
    return d;
}

} // namespace qiftop::backend::linux_
