#include "SockDiagResolver.h"
#include "SockDiagParse.h"
#include "SockDiagDump.h"
#include "ProcSnapshot.h"

#include "backend/Connection.h"

#include <QFile>
#include <QFileInfo>

#include <array>

#include <cerrno>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netinet/in.h>

#include "util/Logging.h"

namespace qiftop::backend::linuximpl {

namespace {

constexpr int kDefaultCacheTtlMs = 1000;
constexpr int kMinCacheTtlMs = 100;

constexpr int safeCacheTtlMs(int raw) noexcept
{
    if (raw <= 0) return kDefaultCacheTtlMs;
    return raw >= kMinCacheTtlMs ? raw : kMinCacheTtlMs;
}

using sockdiag::makeFlowKey;
// Local alias for the historical call site name.
inline auto &makeKey = makeFlowKey;

// Parse /proc/<pid>/status — extract Name (first token, comm) and Uid
// (real uid, first of 4 fields). Returns false on any I/O error.
bool readProcStatus(qint32 pid, QString &comm, quint32 &uid)
{
    QFile f(QStringLiteral("/proc/%1/status").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return false;
    // /proc/<pid>/status is typically <4 KiB.
    const QByteArray data = f.read(4096);
    for (const auto &line : data.split('\n')) {
        if (line.startsWith("Name:")) {
            comm = QString::fromUtf8(line.mid(5).trimmed());
        } else if (line.startsWith("Uid:")) {
            const auto parts = QByteArray(line.mid(4)).simplified().split(' ');
            if (!parts.isEmpty()) uid = parts.first().toUInt();
        }
    }
    return true;
}

QString readProcCmdline(qint32 pid)
{
    QFile f(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray data = f.read(8192);
    // /proc/<pid>/cmdline uses NUL separators between argv entries and
    // typically ends with a NUL. Render as space-joined for display.
    for (auto &b : data) if (b == '\0') b = ' ';
    return QString::fromUtf8(data).trimmed();
}

QString readProcExe(qint32 pid)
{
    return QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
}

} // namespace

struct SockDiagResolver::Impl {
    int           nlFd = -1;
    QElapsedTimer clock;
    qint64        lastDumpMs = -1;
    int           cacheTtlMs = kDefaultCacheTtlMs;

    std::mutex                  mu;
    QHash<QByteArray, quint64>  keyToInode;     // 4-tuple -> kernel inode
    // socket inode -> (owning pid, pid starttime jiffies). starttime is
    // captured at enrollment so a later lookup can detect PID reuse
    // (kernel hands out the same pid to a brand-new process) and skip
    // reading /proc/<pid>/* for the unrelated process.
    struct PidStamp { qint32 pid; quint64 startTime; };
    QHash<quint64, PidStamp>    inodeToPid;
    QHash<qint32, quint64>      pidToStartTime;
    qint64                      lastProcWalkMs = -1;
    bool                        ready = false;
    bool                        warnedAboutEacces = false;
    bool                        warnedAboutCap = false;

    // ----- netlink helpers (delegate to sockdiag:: free functions) ---------

    [[nodiscard]] bool openSocket()
    {
        nlFd = sockdiag::openSockDiagSocket();
        return nlFd >= 0;
    }

    bool dumpProto(quint8 family, quint8 proto)
    {
        const quint32 seq = static_cast<quint32>(clock.elapsed() & 0xffffffff);
        return sockdiag::dumpSocketsViaFd(nlFd, family, proto, keyToInode, seq);
    }

    void refreshSocketTable()
    {
        keyToInode.clear();
        dumpProto(AF_INET,  IPPROTO_TCP);
        dumpProto(AF_INET6, IPPROTO_TCP);
        dumpProto(AF_INET,  IPPROTO_UDP);
        dumpProto(AF_INET6, IPPROTO_UDP);
        qCInfo(lcVerbose) << "SockDiagResolver: socket table refreshed,"
                          << keyToInode.size() << "entries";
    }

    void refreshProcWalk()
    {
        // Repopulate from scratch — PIDs reuse + socket churn make
        // partial diffs error-prone, and a full walk on a typical
        // desktop is < 5 ms. Uses raw POSIX readdir/readlink because
        // Qt's QDir filters skip /proc/<pid>/fd/* entries — they
        // symlink to magic targets like "socket:[N]" that don't
        // resolve to a real inode, so QDir::Files (S_ISREG of target)
        // excludes them.
        inodeToPid.clear();
        pidToStartTime.clear();
        DIR *procDir = ::opendir("/proc");
        if (!procDir) return;
        std::vector<char> linkBuf(256);
        // Track readlink EACCES so we can warn ONCE per process lifetime
        // if /proc/<pid>/fd is unreadable across the board (typical
        // cause: agent has uid=0 but no CAP_SYS_PTRACE, so
        // ptrace_may_access() rejects readlink on every non-root
        // process's fd symlinks → inodeToPid stays empty → every flow
        // reports pid=0 even though process-attribution capability is
        // advertised. Silent until now; one warning is enough — the
        // condition is steady-state, not a once-per-tick race).
        int readlinkOk = 0;
        int readlinkEacces = 0;
        bool capped = false;
        while (auto *de = ::readdir(procDir)) {
            // M9: hard cap — a malicious fd flood (millions of socket
            // fds) must not stall or OOM the root agent. Stop the walk
            // at the cap; the maps are rebuilt from scratch next TTL,
            // so this is the bounded-cache discipline of AGENTS.md §8a
            // rule 8 applied to a per-tick map.
            if (inodeToPid.size() >= sockdiag::kMaxSocketEntries) {
                capped = true;
                break;
            }
            const char *name = de->d_name;
            if (name[0] < '0' || name[0] > '9') continue;
            char *endp = nullptr;
            const long pid = ::strtol(name, &endp, 10);
            if (endp == name || *endp != '\0' || pid <= 0) continue;

            char fdPath[64];
            std::snprintf(fdPath, sizeof(fdPath), "/proc/%ld/fd", pid);
            DIR *fdDir = ::opendir(fdPath);
            if (!fdDir) continue;
            // Snapshot once per pid per refresh, then reuse for every socket fd
            // owned by that pid. The resolve/enrich paths still re-check the
            // current starttime before serving cached attribution.
            const auto st = procsnap::pidStartTime(qint32(pid));
            const quint64 startTime = st.value_or(0);
            while (auto *fde = ::readdir(fdDir)) {
                if (fde->d_name[0] == '.') continue;
                char linkPath[96];
                std::snprintf(linkPath, sizeof(linkPath),
                              "/proc/%ld/fd/%s", pid, fde->d_name);
                for (;;) {
                    const ssize_t n = ::readlink(linkPath, linkBuf.data(),
                                                 linkBuf.size());
                    if (n < 0) {
                        if (errno == EACCES) ++readlinkEacces;
                        break;
                    }
                    ++readlinkOk;
                    if (static_cast<size_t>(n) < linkBuf.size()) {
                        const QString target = QString::fromUtf8(linkBuf.data(),
                                                                 int(n));
                        if (auto inode = sockdiag::parseSocketLink(target)) {
                            // If the pid is already gone, store 0 — resolvePid
                            // will reject it when the guard cannot re-read the
                            // same starttime.
                            inodeToPid.insert(*inode, { qint32(pid), startTime });
                            pidToStartTime.insert(qint32(pid), startTime);
                        }
                        break;
                    }
                    linkBuf.resize(linkBuf.size() * 2);
                }
            }
            ::closedir(fdDir);
        }
        ::closedir(procDir);
        if (capped && !warnedAboutCap) {
            qWarning("SockDiagResolver: /proc fd walk hit the %d-socket "
                     "cap; flows owned by unwalked processes will report "
                     "pid=0 this tick (warning once; suppressing repeats).",
                     sockdiag::kMaxSocketEntries);
            warnedAboutCap = true;
        }
        if (!warnedAboutEacces && readlinkOk == 0 && readlinkEacces > 0) {
            qWarning("SockDiagResolver: every /proc/<pid>/fd readlink failed "
                     "with EACCES (%d attempts). Process attribution will "
                     "report pid=0 for every flow. Most likely cause: the "
                     "agent has uid=0 but the systemd unit's "
                     "CapabilityBoundingSet excludes CAP_SYS_PTRACE — "
                     "ptrace_may_access() requires either matching uid "
                     "or CAP_SYS_PTRACE to follow /proc/<pid>/fd/N symlinks "
                     "for other users' processes.",
                     readlinkEacces);
            warnedAboutEacces = true;
        }
        qCInfo(lcVerbose) << "SockDiagResolver: /proc walk refreshed,"
                          << inodeToPid.size() << "socket fds";
    }

    void maybeRefresh()
    {
        const qint64 now = clock.elapsed();
        if (lastDumpMs < 0 || now - lastDumpMs >= cacheTtlMs) {
            refreshSocketTable();
            lastDumpMs = now;
        }
        if (lastProcWalkMs < 0 || now - lastProcWalkMs >= cacheTtlMs) {
            refreshProcWalk();
            lastProcWalkMs = now;
        }
    }
};

SockDiagResolver::SockDiagResolver(const ResolverTuning &tuning)
    : m_d(std::make_unique<Impl>())
{
    m_d->cacheTtlMs = safeCacheTtlMs(tuning.cacheRefreshMs);
    m_d->clock.start();
}

SockDiagResolver::~SockDiagResolver()
{
    if (m_d->nlFd >= 0) ::close(m_d->nlFd);
}

bool SockDiagResolver::initialize()
{
    if (!m_d->openSocket()) return false;
    // Probe dump: if the kernel rejects sock_diag entirely (CONFIG_INET_DIAG
    // disabled — very rare) we should report failure NOW rather than first
    // call site, so the resolver advertises no capability and the UI hides
    // attribution cleanly.
    std::scoped_lock lock(m_d->mu);
    if (!m_d->dumpProto(AF_INET, IPPROTO_TCP)) {
        ::close(m_d->nlFd);
        m_d->nlFd = -1;
        return false;
    }
    m_d->ready      = true;
    // Leave lastDumpMs at its sentinel so the first resolvePid() call
    // triggers a fresh dump. The probe dump above only loaded TCPv4 and
    // is meant strictly for capability detection.
    m_d->keyToInode.clear();
    qCInfo(lcVerbose) << "SockDiagResolver: ready (probe ok)";
    return true;
}

QStringList SockDiagResolver::capabilities() const
{
    if (!m_d->ready) return {};
    return { QStringLiteral("process-attribution") };
}

qint32 SockDiagResolver::resolvePid(const Connection &flow)
{
    if (!m_d->ready) return 0;
    const quint8 proto = (flow.proto == L4Proto::Tcp)
        ? quint8{IPPROTO_TCP}
        : (flow.proto == L4Proto::Udp ? quint8{IPPROTO_UDP} : quint8{0});
    if (proto == 0) return 0;

    std::scoped_lock lock(m_d->mu);
    m_d->maybeRefresh();

    // Try the full 4-tuple first (connected sockets / established TCP). UDP
    // then gets a local-only fallback for unconnected sockets: exact local
    // addr+port, then wildcard bind (0.0.0.0 / ::) on the same port. TCP
    // listeners are deliberately excluded from the local fallback: listener
    // collisions are common and live TCP flows should have a full tuple.
    auto itSock = m_d->keyToInode.constFind(
        makeKey(proto, flow.local.address, flow.local.port,
                flow.remote.address, flow.remote.port));
    if (itSock == m_d->keyToInode.constEnd() && proto == IPPROTO_UDP) {
        const auto itLocal = m_d->keyToInode.constFind(
            sockdiag::makeLocalKey(proto, flow.local.address, flow.local.port));
        const bool v6 = flow.local.address.protocol() == QAbstractSocket::IPv6Protocol;
        const QHostAddress anyAddr(v6 ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4);
        const auto itWildcard = m_d->keyToInode.constFind(
            sockdiag::makeLocalKey(proto, anyAddr, flow.local.port));

        if (itLocal != m_d->keyToInode.constEnd()) {
            if (itWildcard != m_d->keyToInode.constEnd()
                && (sockdiag::isAmbiguousLocalInode(*itWildcard)
                    || *itWildcard != *itLocal)) {
                return 0;
            }
            itSock = itLocal;
        } else {
            itSock = itWildcard;
        }
    }
    if (itSock == m_d->keyToInode.constEnd()) return 0;
    if (sockdiag::isAmbiguousLocalInode(*itSock)) return 0;
    auto itPid = m_d->inodeToPid.constFind(*itSock);
    if (itPid == m_d->inodeToPid.constEnd()) return 0;

    // Defend against PID reuse: if the pid's starttime has changed
    // since we enrolled it, a different process now holds that pid
    // and any later /proc/<pid>/* we'd read would be wrong.
    const auto stNow = procsnap::pidStartTime(itPid->pid);
    if (!stNow.has_value() || *stNow != itPid->startTime) {
        return 0;
    }

    return itPid->pid;
}

std::optional<ProcessInfo> SockDiagResolver::enrichPid(qint32 pid)
{
    if (!m_d->ready || pid <= 0) return std::nullopt;

    {
        std::scoped_lock lock(m_d->mu);
        m_d->maybeRefresh();
        auto it = m_d->pidToStartTime.constFind(pid);
        if (it == m_d->pidToStartTime.constEnd()) return std::nullopt;

        // Re-check PID identity immediately before /proc metadata reads.
        // This keeps the resolvePid/enrichPid split safe against PID reuse
        // while still allowing the caller to memoise enrichment per PID.
        const auto stNow = procsnap::pidStartTime(pid);
        if (!stNow.has_value() || *stNow != it.value()) {
            return std::nullopt;
        }
    }

    ProcessInfo info;
    info.pid = pid;
    readProcStatus(info.pid, info.comm, info.uid);
    info.cmdline = readProcCmdline(info.pid);
    info.exe     = readProcExe(info.pid);
    return info;
}

} // namespace qiftop::backend::linuximpl
