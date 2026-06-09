#include "NetnsScanner.h"
#include "SockDiagDump.h"
#include "SockDiagParse.h"
#include "ProcSnapshot.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>          // setns(2), CLONE_NEWNET
#include <sys/stat.h>
#include <sys/syscall.h>    // SYS_gettid
#include <unistd.h>

#include <netinet/in.h>

#include "backend/Connection.h"
#include "util/Logging.h"

namespace qiftop::backend::linuximpl {

namespace {

constexpr int kRefreshIntervalMs = 5000;   // netns set changes are rare
constexpr int kMaxNetnsesPerTick = 256;    // sanity cap

// Read /proc/<pid>/ns/net symlink target ("net:[NNNN]") and extract
// the inode number. Returns nullopt on any failure.
std::optional<quint64> readNetnsInode(long pid)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/ns/net", pid);
    std::vector<char> buf(64);
    for (;;) {
        const ssize_t n = ::readlink(path, buf.data(), buf.size());
        if (n < 0) return std::nullopt;
        if (static_cast<size_t>(n) < buf.size()) {
            return sockdiag::parseNamespaceLink(
                QString::fromUtf8(buf.data(), int(n)),
                QLatin1StringView("net"));
        }
        buf.resize(buf.size() * 2);
    }
}

// RAII guard: restores the calling thread's netns to the captured
// anchor fd in its destructor. Restoration failure is catastrophic —
// any future syscall by this thread would target the wrong netns —
// so we qFatal rather than carry on.
class NetnsRestoreGuard
{
public:
    explicit NetnsRestoreGuard(int anchorFd) : m_anchorFd(anchorFd) {}
    NetnsRestoreGuard(const NetnsRestoreGuard &) = delete;
    NetnsRestoreGuard &operator=(const NetnsRestoreGuard &) = delete;
    ~NetnsRestoreGuard()
    {
        if (m_anchorFd < 0) return;
        if (::setns(m_anchorFd, CLONE_NEWNET) != 0) {
            // Stuck in a stranger's netns. Future host dumps would
            // attribute every host flow to whatever container we last
            // visited. Refuse to continue.
            qFatal("NetnsScanner: setns(anchor) failed (%s) — aborting "
                   "agent to avoid mis-attributing host flows.",
                   std::strerror(errno));
        }
    }
private:
    int m_anchorFd;
};

} // namespace

// ===========================================================================
// Worker (runs on the dedicated NetnsScanner QThread, never on data thread).
// ===========================================================================

class NetnsScannerWorker : public QObject
{
    Q_OBJECT
public:
    explicit NetnsScannerWorker(NetnsScanner *owner) : m_owner(owner) {}

    void setStop(std::atomic<bool> *stop) { m_stop = stop; }

public slots:
    void start()
    {
        m_anchorFd = ::open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
        if (m_anchorFd < 0) {
            qCWarning(lcVerbose) << "NetnsScannerWorker: cannot open anchor"
                                    " /proc/self/ns/net:" << std::strerror(errno);
            return;
        }
        struct stat st{};
        if (::fstat(m_anchorFd, &st) != 0) {
            qCWarning(lcVerbose) << "NetnsScannerWorker: fstat anchor:"
                                 << std::strerror(errno);
            ::close(m_anchorFd);
            m_anchorFd = -1;
            return;
        }
        m_anchorInode = st.st_ino;

        m_timer = new QTimer(this);
        m_timer->setInterval(kRefreshIntervalMs);
        connect(m_timer, &QTimer::timeout, this, &NetnsScannerWorker::tick);
        QTimer::singleShot(0, this, &NetnsScannerWorker::tick);  // warm up
        m_timer->start();
    }

    void stop()
    {
        if (m_timer) { m_timer->stop(); m_timer = nullptr; }
        if (m_anchorFd >= 0) { ::close(m_anchorFd); m_anchorFd = -1; }
    }

private slots:
    void tick()
    {
        if (m_stop && m_stop->load()) return;
        if (m_anchorFd < 0)            return;

        // 1) Walk /proc, collect distinct (netns inode → representative pid).
        std::vector<std::pair<quint64, long>> netnses;
        netnses.reserve(8);
        std::set<quint64> seen;
        DIR *procDir = ::opendir("/proc");
        if (!procDir) return;
        while (auto *de = ::readdir(procDir)) {
            const char *nm = de->d_name;
            if (nm[0] < '0' || nm[0] > '9') continue;
            char *endp = nullptr;
            const long pid = ::strtol(nm, &endp, 10);
            if (endp == nm || *endp != '\0' || pid <= 0) continue;
            const auto ino = readNetnsInode(pid);
            if (!ino || *ino == m_anchorInode) continue;
            if (!seen.insert(*ino).second) continue;
            netnses.emplace_back(*ino, pid);
            if (int(netnses.size()) >= kMaxNetnsesPerTick) break;
        }
        ::closedir(procDir);

        // 2) For each non-host netns, dump sock_diag + walk /proc-in-ns.
        QHash<QByteArray, quint64>                          mergedKey;
        QHash<quint64, NetnsScanner::PidStamp>              mergedInode;
        for (const auto &[nsIno, repPid] : netnses) {
            scanOneNetns(nsIno, repPid, mergedKey, mergedInode);
        }

        // 3) Publish atomically.
        {
            std::lock_guard lk(m_owner->m_mu);
            m_owner->m_keyToInode  = std::move(mergedKey);
            m_owner->m_inodeToPid  = std::move(mergedInode);
        }
        qCInfo(lcVerbose) << "NetnsScanner: refreshed across" << netnses.size()
                          << "non-host netns(es)";
    }

private:
    void scanOneNetns(quint64 nsIno, long repPid,
                      QHash<QByteArray, quint64>             &outKey,
                      QHash<quint64, NetnsScanner::PidStamp> &outInode)
    {
        char nsPath[96];
        std::snprintf(nsPath, sizeof(nsPath), "/proc/%ld/ns/net", repPid);
        const int targetFd = ::open(nsPath, O_RDONLY | O_CLOEXEC);
        if (targetFd < 0) {
            // pid vanished before we got here — silent skip.
            return;
        }
        // setns into target. If this fails (e.g. EPERM on a host with
        // no CAP_SYS_ADMIN, or ENOENT racing a destroyed netns), we
        // skip — the guard is constructed AFTER a successful setns so
        // its destructor won't try to undo a no-op restore.
        if (::setns(targetFd, CLONE_NEWNET) != 0) {
            const int err = errno;
            ::close(targetFd);
            // Log only the unexpected errors — ENOENT/EPERM are routine.
            if (err != ENOENT && err != EPERM) {
                qCWarning(lcVerbose) << "NetnsScanner: setns(target) failed:"
                                     << std::strerror(err);
            }
            return;
        }
        // From here on, this thread is in the container netns. Anything
        // can fail; the guard MUST run.
        NetnsRestoreGuard guard(m_anchorFd);

        // 2a) Fresh netlink fd in this netns (cannot reuse a host fd).
        const int nlFd = sockdiag::openSockDiagSocket();
        if (nlFd >= 0) {
            QHash<QByteArray, quint64> nsKeyToInode;
            (void)sockdiag::dumpSocketsViaFd(nlFd, AF_INET,  IPPROTO_TCP, nsKeyToInode, 1);
            (void)sockdiag::dumpSocketsViaFd(nlFd, AF_INET6, IPPROTO_TCP, nsKeyToInode, 2);
            (void)sockdiag::dumpSocketsViaFd(nlFd, AF_INET,  IPPROTO_UDP, nsKeyToInode, 3);
            (void)sockdiag::dumpSocketsViaFd(nlFd, AF_INET6, IPPROTO_UDP, nsKeyToInode, 4);
            ::close(nlFd);

            // 2b) Walk /proc to find pids whose netns inode == nsIno;
            //     for those pids, scan /proc/<pid>/fd and build
            //     inode→pid pairs.
            QHash<quint64, NetnsScanner::PidStamp> nsInodeToPid;
            buildInodeToPidForNetns(nsIno, nsInodeToPid);

            // 2c) Merge: only flow tuples whose socket inode resolves
            //     to a pid in this netns are useful (otherwise the
            //     inode is the kernel's, no owner).
            for (auto it = nsKeyToInode.constBegin();
                 it != nsKeyToInode.constEnd(); ++it)
            {
                if (nsInodeToPid.contains(it.value())) {
                    outKey.insert(it.key(), it.value());
                }
            }
            for (auto it = nsInodeToPid.constBegin();
                 it != nsInodeToPid.constEnd(); ++it)
            {
                outInode.insert(it.key(), it.value());
            }
        }

        ::close(targetFd);
        // guard dtor restores anchor netns; qFatal on failure.
    }

    // Walk /proc; for every pid whose /proc/<pid>/ns/net inode equals
    // nsIno, scan /proc/<pid>/fd and record socket-inode → (pid, starttime).
    // NOTE: at this point THIS THREAD is INSIDE the container's netns,
    // but /proc is a mount-namespace concern, NOT a netns concern — we
    // still see the host's full /proc tree because we never changed
    // mount namespaces. That's exactly what we want.
    void buildInodeToPidForNetns(
        quint64 nsIno,
        QHash<quint64, NetnsScanner::PidStamp> &out)
    {
        DIR *procDir = ::opendir("/proc");
        if (!procDir) return;
        std::vector<char> linkBuf(256);
        while (auto *de = ::readdir(procDir)) {
            const char *nm = de->d_name;
            if (nm[0] < '0' || nm[0] > '9') continue;
            char *endp = nullptr;
            const long pid = ::strtol(nm, &endp, 10);
            if (endp == nm || *endp != '\0' || pid <= 0) continue;
            const auto ino = readNetnsInode(pid);
            if (!ino || *ino != nsIno) continue;

            const auto st = procsnap::pidStartTime(qint32(pid));
            const quint64 startTime = st.value_or(0);

            char fdPath[64];
            std::snprintf(fdPath, sizeof(fdPath), "/proc/%ld/fd", pid);
            DIR *fdDir = ::opendir(fdPath);
            if (!fdDir) continue;
            while (auto *fde = ::readdir(fdDir)) {
                if (fde->d_name[0] == '.') continue;
                char linkPath[96];
                std::snprintf(linkPath, sizeof(linkPath),
                              "/proc/%ld/fd/%s", pid, fde->d_name);
                for (;;) {
                    const ssize_t n = ::readlink(linkPath, linkBuf.data(),
                                                 linkBuf.size());
                    if (n < 0) break;
                    if (static_cast<size_t>(n) < linkBuf.size()) {
                        const QString target =
                            QString::fromUtf8(linkBuf.data(), int(n));
                        if (auto inode = sockdiag::parseSocketLink(target)) {
                            out.insert(*inode,
                                       { qint32(pid), startTime });
                        }
                        break;
                    }
                    linkBuf.resize(linkBuf.size() * 2);
                }
            }
            ::closedir(fdDir);
        }
        ::closedir(procDir);
    }

private:
    NetnsScanner       *m_owner       = nullptr;
    std::atomic<bool>  *m_stop        = nullptr;
    QTimer             *m_timer       = nullptr;
    int                 m_anchorFd    = -1;
    quint64             m_anchorInode = 0;
};

// ===========================================================================
// NetnsScanner (public API; lives on the agent main thread).
// ===========================================================================

NetnsScanner::NetnsScanner() = default;

NetnsScanner::~NetnsScanner()
{
    m_stop.store(true);
    if (m_thread) {
        if (m_worker) {
            QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
        }
        m_thread->quit();
        m_thread->wait(2000);
        delete m_thread;
        m_thread = nullptr;
        m_worker = nullptr;  // owned by thread
    }
}

bool NetnsScanner::initialize()
{
    // Capability probe: we need to be able to open /proc/self/ns/net.
    // setns() requires CAP_SYS_ADMIN; we test it lazily on the first
    // tick (a failure there silently disables a single netns rather
    // than the whole resolver). That keeps the resolver usable even
    // on hosts where /proc is readable but CAP_SYS_ADMIN is dropped —
    // it just won't actually discover any non-host sockets.
    QFile self(QStringLiteral("/proc/self/ns/net"));
    if (!self.exists()) {
        qCWarning(lcVerbose) << "NetnsScanner: /proc/self/ns/net missing;"
                                " netns scan disabled";
        return false;
    }

    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("qiftop-netns"));
    m_worker = new NetnsScannerWorker(this);
    m_worker->moveToThread(m_thread);
    m_worker->setStop(&m_stop);
    QObject::connect(m_thread, &QThread::started,
                     m_worker, &NetnsScannerWorker::start);
    QObject::connect(m_thread, &QThread::finished,
                     m_worker, &QObject::deleteLater);
    m_thread->start();

    m_ready = true;
    qCInfo(lcVerbose) << "NetnsScanner: worker thread started";
    return true;
}

QStringList NetnsScanner::capabilities() const
{
    if (!m_ready) return {};
    return { QStringLiteral("netns-scan") };
}

std::optional<ProcessInfo>
NetnsScanner::resolveFlow(const Connection &flow)
{
    if (!m_ready) return std::nullopt;
    const quint8 proto = (flow.proto == L4Proto::Tcp) ? quint8{IPPROTO_TCP}
                       : (flow.proto == L4Proto::Udp ? quint8{IPPROTO_UDP}
                                                     : quint8{0});
    if (proto == 0) return std::nullopt;

    const QByteArray key = sockdiag::makeFlowKey(
        proto,
        flow.local.address,  flow.local.port,
        flow.remote.address, flow.remote.port);

    qint32  pid       = 0;
    quint64 startTime = 0;
    {
        std::lock_guard lk(m_mu);
        auto itSock = m_keyToInode.constFind(key);
        if (itSock == m_keyToInode.constEnd()) return std::nullopt;
        auto itPid = m_inodeToPid.constFind(*itSock);
        if (itPid == m_inodeToPid.constEnd()) return std::nullopt;
        pid       = itPid->pid;
        startTime = itPid->startTime;
    }

    // PID-reuse guard. See AGENTS.md §8a rule 2.
    const auto stNow = procsnap::pidStartTime(pid);
    if (!stNow.has_value() || *stNow != startTime) return std::nullopt;

    ProcessInfo info;
    info.pid = pid;
    // Minimal read — comm only. Full enrichment (cmdline/exe) lives in
    // SockDiagResolver; for cross-netns attribution the pid + container
    // badge from CgroupClassifier is the usual identifier the user
    // actually wants.
    QFile status(QStringLiteral("/proc/%1/status").arg(pid));
    if (status.open(QIODevice::ReadOnly)) {
        const QByteArray data = status.read(4096);
        for (const auto &line : data.split('\n')) {
            if (line.startsWith("Name:")) {
                info.comm = QString::fromUtf8(line.mid(5).trimmed());
                break;
            }
        }
    }
    return info;
}

} // namespace qiftop::backend::linuximpl

#include "NetnsScanner.moc"
