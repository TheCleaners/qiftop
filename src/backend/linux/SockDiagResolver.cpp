#include "SockDiagResolver.h"
#include "SockDiagParse.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <array>
#include "ProcSnapshot.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>

#include "util/Logging.h"

namespace qiftop::backend::linuximpl {

namespace {

constexpr int kCacheTtlMs = 1000;

// Compose a stable lookup key from a 4-tuple. proto is the IANA number
// (TCP=6, UDP=17). Order matters: must match what we generate for
// resolveFlow() lookups and what we record from sock_diag dumps.
QByteArray makeKey(quint8 proto,
                   const QHostAddress &localAddr, quint16 localPort,
                   const QHostAddress &remoteAddr, quint16 remotePort)
{
    QByteArray k;
    k.reserve(48);
    k.append(static_cast<char>(proto));
    const auto append = [&](const QHostAddress &a, quint16 port) {
        if (a.protocol() == QAbstractSocket::IPv6Protocol) {
            Q_IPV6ADDR v6 = a.toIPv6Address();
            k.append(reinterpret_cast<const char*>(&v6), sizeof(v6));
        } else {
            quint32 v4 = qToBigEndian(a.toIPv4Address());
            k.append(reinterpret_cast<const char*>(&v4), sizeof(v4));
        }
        quint16 p = qToBigEndian(port);
        k.append(reinterpret_cast<const char*>(&p), sizeof(p));
    };
    append(localAddr, localPort);
    append(remoteAddr, remotePort);
    return k;
}

// Build a QHostAddress from inet_diag_sockid's raw IDIAG_FAM-typed
// address bytes. v4 lives in id_src[0]; v6 uses all 16 bytes.
QHostAddress addrFromDiag(quint8 family, const __be32 buf[4])
{
    if (family == AF_INET) {
        return QHostAddress(qFromBigEndian<quint32>(buf[0]));
    }
    Q_IPV6ADDR v6;
    std::memcpy(&v6, buf, sizeof(v6));
    return QHostAddress(v6);
}

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

    std::mutex                  mu;
    QHash<QByteArray, quint64>  keyToInode;     // 4-tuple -> kernel inode
    // socket inode -> (owning pid, pid starttime jiffies). starttime is
    // captured at enrollment so a later lookup can detect PID reuse
    // (kernel hands out the same pid to a brand-new process) and skip
    // reading /proc/<pid>/* for the unrelated process.
    struct PidStamp { qint32 pid; quint64 startTime; };
    QHash<quint64, PidStamp>    inodeToPid;
    qint64                      lastProcWalkMs = -1;
    bool                        ready = false;

    // ----- netlink helpers -------------------------------------------------

    [[nodiscard]] bool openSocket()
    {
        nlFd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
        if (nlFd < 0) {
            qCWarning(lcVerbose) << "SockDiagResolver: socket(NETLINK_SOCK_DIAG):"
                                 << std::strerror(errno);
            return false;
        }
        // Bind to the kernel (port=0); not strictly required but lets us
        // keep replies straight if we ever multi-thread the dump.
        sockaddr_nl sa{};
        sa.nl_family = AF_NETLINK;
        if (::bind(nlFd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            qCWarning(lcVerbose) << "SockDiagResolver: bind:" << std::strerror(errno);
            ::close(nlFd);
            nlFd = -1;
            return false;
        }
        return true;
    }

    bool dumpProto(quint8 family, quint8 proto)
    {
        struct {
            nlmsghdr           nlh;
            inet_diag_req_v2   req;
        } msg{};
        msg.nlh.nlmsg_len   = sizeof(msg);
        msg.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
        msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        msg.nlh.nlmsg_seq   = static_cast<__u32>(clock.elapsed() & 0xffffffff);
        msg.req.sdiag_family   = family;
        msg.req.sdiag_protocol = proto;
        // TCP: every state; UDP: there's only one "state" but the kernel
        // requires the mask to be non-zero. Setting all bits is the
        // canonical idiom from ss(8).
        msg.req.idiag_states   = ~0u;
        msg.req.idiag_ext      = 0;
        msg.req.id             = {};

        if (::send(nlFd, &msg, sizeof(msg), 0) < 0) {
            qCWarning(lcVerbose) << "SockDiagResolver: send:" << std::strerror(errno);
            return false;
        }

        std::array<char, 16384> buf{};
        for (;;) {
            ssize_t n = ::recv(nlFd, buf.data(), buf.size(), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                qCWarning(lcVerbose) << "SockDiagResolver: recv:" << std::strerror(errno);
                return false;
            }
            if (n == 0) return true;
            for (auto *nh = reinterpret_cast<nlmsghdr*>(buf.data());
                 NLMSG_OK(nh, n);
                 nh = NLMSG_NEXT(nh, n)) {
                if (nh->nlmsg_type == NLMSG_DONE) return true;
                if (nh->nlmsg_type == NLMSG_ERROR) {
                    const auto *err = static_cast<nlmsgerr*>(NLMSG_DATA(nh));
                    if (err->error != 0) {
                        qCWarning(lcVerbose) << "SockDiagResolver: NLMSG_ERROR:"
                                             << std::strerror(-err->error);
                        return false;
                    }
                    return true;
                }
                if (nh->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;
                if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(inet_diag_msg))) continue;
                const auto *m = static_cast<const inet_diag_msg*>(NLMSG_DATA(nh));
                if (m->idiag_inode == 0) continue;
                const auto local  = addrFromDiag(m->idiag_family, m->id.idiag_src);
                const auto remote = addrFromDiag(m->idiag_family, m->id.idiag_dst);
                const quint16 lport = qFromBigEndian(m->id.idiag_sport);
                const quint16 rport = qFromBigEndian(m->id.idiag_dport);
                keyToInode.insert(makeKey(proto, local, lport, remote, rport),
                                  m->idiag_inode);
            }
        }
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
        DIR *procDir = ::opendir("/proc");
        if (!procDir) return;
        std::vector<char> linkBuf(256);
        while (auto *de = ::readdir(procDir)) {
            const char *name = de->d_name;
            if (name[0] < '0' || name[0] > '9') continue;
            char *endp = nullptr;
            const long pid = ::strtol(name, &endp, 10);
            if (endp == name || *endp != '\0' || pid <= 0) continue;

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
                        const QString target = QString::fromUtf8(linkBuf.data(),
                                                                 int(n));
                        if (auto inode = sockdiag::parseSocketLink(target)) {
                            // Snapshot starttime now; verifies later.
                            // If the pid is already gone, store 0 —
                            // resolveFlow will treat any nonzero
                            // mismatch as reuse.
                            const auto st = procsnap::pidStartTime(qint32(pid));
                            inodeToPid.insert(*inode,
                                              { qint32(pid), st.value_or(0) });
                        }
                        break;
                    }
                    linkBuf.resize(linkBuf.size() * 2);
                }
            }
            ::closedir(fdDir);
        }
        ::closedir(procDir);
        qCInfo(lcVerbose) << "SockDiagResolver: /proc walk refreshed,"
                          << inodeToPid.size() << "socket fds";
    }

    void maybeRefresh()
    {
        const qint64 now = clock.elapsed();
        if (lastDumpMs < 0 || now - lastDumpMs >= kCacheTtlMs) {
            refreshSocketTable();
            lastDumpMs = now;
        }
        if (lastProcWalkMs < 0 || now - lastProcWalkMs >= kCacheTtlMs) {
            refreshProcWalk();
            lastProcWalkMs = now;
        }
    }
};

SockDiagResolver::SockDiagResolver() : m_d(std::make_unique<Impl>())
{
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
    std::lock_guard lock(m_d->mu);
    if (!m_d->dumpProto(AF_INET, IPPROTO_TCP)) {
        ::close(m_d->nlFd);
        m_d->nlFd = -1;
        return false;
    }
    m_d->ready      = true;
    // Leave lastDumpMs at its sentinel so the first resolveFlow() call
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

std::optional<ProcessInfo> SockDiagResolver::resolveFlow(const Connection &flow)
{
    if (!m_d->ready) return std::nullopt;
    const quint8 proto = (flow.proto == L4Proto::Tcp)
        ? quint8{IPPROTO_TCP}
        : (flow.proto == L4Proto::Udp ? quint8{IPPROTO_UDP} : quint8{0});
    if (proto == 0) return std::nullopt;

    std::lock_guard lock(m_d->mu);
    m_d->maybeRefresh();

    const QByteArray key = makeKey(proto,
                                   flow.local.address,  flow.local.port,
                                   flow.remote.address, flow.remote.port);
    auto itSock = m_d->keyToInode.constFind(key);
    if (itSock == m_d->keyToInode.constEnd()) return std::nullopt;
    auto itPid = m_d->inodeToPid.constFind(*itSock);
    if (itPid == m_d->inodeToPid.constEnd()) return std::nullopt;

    // Defend against PID reuse: if the pid's starttime has changed
    // since we enrolled it, a different process now holds that pid
    // and any /proc/<pid>/* we'd read would be wrong. Drop the
    // attribution rather than mislabel the flow.
    const auto stNow = procsnap::pidStartTime(itPid->pid);
    if (!stNow.has_value() || *stNow != itPid->startTime) {
        return std::nullopt;
    }

    ProcessInfo info;
    info.pid = itPid->pid;
    readProcStatus(info.pid, info.comm, info.uid);
    info.cmdline = readProcCmdline(info.pid);
    info.exe     = readProcExe(info.pid);
    return info;
}

} // namespace qiftop::backend::linuximpl
