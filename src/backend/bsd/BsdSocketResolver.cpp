#include "BsdSocketResolver.h"
#include "BsdFlowKey.h"

#include "util/Logging.h"

#if defined(__NetBSD__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include <QtEndian>

#if defined(__NetBSD__)
#include <sys/file.h>      // DTYPE_SOCKET
// kinfo_pcb is in <sys/socket.h>; kinfo_file/KERN_FILE2 in <sys/sysctl.h>.
#elif defined(__FreeBSD__)
#include <sys/user.h>      // kinfo_proc, kinfo_file, KF_TYPE_SOCKET
#endif

namespace qiftop::backend::bsd {

namespace {

// Fetch a variable-size sysctl array, growing the buffer until it fits.
// A size-probe (oldp == NULL) returns 0 and reports the needed size in `sz`
// (NOT ENOMEM), so we must keep iterating after the probe and only stop once
// we've read into a non-null buffer. Mirrors sockstat's sysctl_sucker.
std::vector<char> suckMib(int *name, u_int namelen)
{
    std::vector<char> buf;
    size_t sz = 0;
    for (;;) {
        void *old = buf.empty() ? nullptr : buf.data();
        const int rc = sysctl(name, namelen, old, &sz, nullptr, 0);
        if (rc == -1 && errno != ENOMEM) {
            qCInfo(lcVerbose).noquote()
                << "bsd-resolver: sysctl failed:" << strerror(errno);
            return {};
        }
        // Done when we've read into a real buffer, OR the size probe reports
        // nothing to fetch (sz == 0). The latter guard is essential: without
        // it a 0-byte result (e.g. KERN_PROC_FILEDESC for a process with no
        // open files) leaves buf empty → old stays nullptr → infinite loop.
        if (rc == 0 && (old != nullptr || sz == 0))
            break;
        buf.resize(sz);
    }
    buf.resize(sz);
    return buf;
}

Endpoint endpointFromSockaddr(const sockaddr *sa)
{
    Endpoint ep;
    if (!sa)
        return ep;
    if (sa->sa_family == AF_INET) {
        const auto *sin = reinterpret_cast<const sockaddr_in *>(sa);
        ep.address = QHostAddress(qFromBigEndian<quint32>(&sin->sin_addr.s_addr));
        ep.port    = qFromBigEndian<quint16>(&sin->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(sa);
        Q_IPV6ADDR raw;
        memcpy(&raw, &sin6->sin6_addr, 16);
        ep.address = QHostAddress(raw);
        ep.port    = qFromBigEndian<quint16>(&sin6->sin6_port);
    }
    return ep;
}

} // namespace
} // namespace qiftop::backend::bsd
#endif // __NetBSD__ || __FreeBSD__

namespace qiftop::backend::bsd {

// Shared lookup: exact 4-tuple first, then the local 2-tuple fallback. On
// platforms without a resolver implementation both maps are empty, so this
// safely returns an invalid ProcessInfo.
ProcessInfo BsdSocketResolver::lookup(L4Proto proto, const Endpoint &local,
                                      const Endpoint &remote) const
{
    if (auto it = m_exact.constFind(flowKeyExact(proto, local, remote));
        it != m_exact.constEnd())
        return it.value();
    if (auto it = m_local.constFind(flowKeyLocal(proto, local));
        it != m_local.constEnd())
        return it.value();
    return {};
}

#if defined(__NetBSD__)

namespace {

// pid -> (comm, uid) via KERN_PROC2 / KERN_PROC_ALL.
QHash<qint32, ProcessInfo> buildPidInfo()
{
    QHash<qint32, ProcessInfo> out;
    int name[6] = {CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0,
                   sizeof(struct kinfo_proc2), INT_MAX};
    const std::vector<char> buf = suckMib(name, 6);
    const size_t n = buf.size() / sizeof(struct kinfo_proc2);
    const auto *procs = reinterpret_cast<const struct kinfo_proc2 *>(buf.data());
    for (size_t i = 0; i < n; ++i) {
        ProcessInfo info;
        info.pid  = static_cast<qint32>(procs[i].p_pid);
        info.uid  = static_cast<quint32>(procs[i].p_ruid);
        info.comm = QString::fromUtf8(procs[i].p_comm);
        out.insert(info.pid, info);
    }
    return out;
}

// socket kernel pointer -> owning pid via KERN_FILE2 / KERN_FILE_BYPID.
QHash<quint64, qint32> buildSocketPid()
{
    QHash<quint64, qint32> out;
    int name[6] = {CTL_KERN, KERN_FILE2, KERN_FILE_BYPID, 0,
                   sizeof(struct kinfo_file), INT_MAX};
    const std::vector<char> buf = suckMib(name, 6);
    const size_t n = buf.size() / sizeof(struct kinfo_file);
    const auto *files = reinterpret_cast<const struct kinfo_file *>(buf.data());
    for (size_t i = 0; i < n; ++i) {
        const struct kinfo_file &f = files[i];
        if (f.ki_ftype != DTYPE_SOCKET || f.ki_fdata == 0 || f.ki_pid == 0)
            continue;
        out.insert(static_cast<quint64>(f.ki_fdata), static_cast<qint32>(f.ki_pid));
    }
    return out;
}

} // namespace

void BsdSocketResolver::refresh()
{
    m_exact.clear();
    m_local.clear();

    const QHash<qint32, ProcessInfo> pidInfo   = buildPidInfo();
    const QHash<quint64, qint32>     socketPid = buildSocketPid();

    struct ProtoList { const char *name; L4Proto proto; };
    static const ProtoList kLists[] = {
        {"net.inet.tcp.pcblist",   L4Proto::Tcp},
        {"net.inet.udp.pcblist",   L4Proto::Udp},
        {"net.inet6.tcp6.pcblist", L4Proto::Tcp},
        {"net.inet6.udp6.pcblist", L4Proto::Udp},
    };

    for (const auto &pl : kLists) {
        int name[CTL_MAXNAME];
        size_t depth = CTL_MAXNAME;
        if (sysctlnametomib(pl.name, name, &depth) != 0)
            continue;
        u_int namelen = static_cast<u_int>(depth);
        name[namelen++] = PCB_ALL;
        name[namelen++] = 0;
        name[namelen++] = sizeof(struct kinfo_pcb);
        name[namelen++] = INT_MAX;

        const std::vector<char> buf = suckMib(name, namelen);
        const size_t n = buf.size() / sizeof(struct kinfo_pcb);
        const auto *pcbs = reinterpret_cast<const struct kinfo_pcb *>(buf.data());
        for (size_t i = 0; i < n; ++i) {
            const struct kinfo_pcb &pcb = pcbs[i];
            const auto pidIt = socketPid.constFind(static_cast<quint64>(pcb.ki_sockaddr));
            if (pidIt == socketPid.constEnd())
                continue;
            const auto infoIt = pidInfo.constFind(pidIt.value());
            if (infoIt == pidInfo.constEnd())
                continue;

            const Endpoint local  = endpointFromSockaddr(
                reinterpret_cast<const sockaddr *>(&pcb.ki_src));
            const Endpoint remote = endpointFromSockaddr(
                reinterpret_cast<const sockaddr *>(&pcb.ki_dst));
            if (local.address.isNull())
                continue;

            const ProcessInfo &info = infoIt.value();
            if (!remote.address.isNull() && remote.port != 0)
                m_exact.insert(flowKeyExact(pl.proto, local, remote), info);
            const QString lk = flowKeyLocal(pl.proto, local);
            if (!m_local.contains(lk))
                m_local.insert(lk, info);
        }
    }
}

#elif defined(__FreeBSD__)

// FreeBSD socket -> process attribution via the ABI-stable kinfo_file path
// (what libprocstat / sockstat(1) use). Unlike NetBSD there is no socket-ptr
// join: KERN_PROC_FILEDESC returns kinfo_file records that already carry the
// socket's local/peer sockaddrs (kf_sa_local/kf_sa_peer) for KF_TYPE_SOCKET
// fds, and we know the owning pid because the query is per-pid. We avoid
// xinpcb/pcblist entirely — those structs are NOT ABI-stable and drift between
// kernel and installed headers (observed on FreeBSD 14: kernel xinpcb 744 B vs
// header 400 B), whereas kinfo_file is length-prefixed (kf_structsize) and
// offset-stable by design.
//
// Reading another process's fds requires privilege; qiftop's capture already
// runs as root (for /dev/bpf), so the resolver sees every process.

void BsdSocketResolver::refresh()
{
    m_exact.clear();
    m_local.clear();

    // 1. pid -> (comm, uid) from KERN_PROC_PROC (kinfo_proc, ki_structsize stride).
    QHash<qint32, ProcessInfo> pidInfo;
    {
        int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
        const std::vector<char> buf = suckMib(mib, 3);
        const char *p = buf.data();
        const char *end = p + buf.size();
        while (p + static_cast<long>(sizeof(int)) <= end) {
            const auto *kp = reinterpret_cast<const struct kinfo_proc *>(p);
            const int sz = kp->ki_structsize;
            if (sz <= 0)
                break;
            ProcessInfo info;
            info.pid  = static_cast<qint32>(kp->ki_pid);
            info.uid  = static_cast<quint32>(kp->ki_ruid);
            info.comm = QString::fromUtf8(kp->ki_comm);
            pidInfo.insert(info.pid, info);
            p += sz;
        }
    }

    // 2. Per pid: KERN_PROC_FILEDESC -> kinfo_file (kf_structsize stride). For
    //    each connected INET/INET6 TCP/UDP socket, stamp its 5-tuple.
    for (auto it = pidInfo.constBegin(); it != pidInfo.constEnd(); ++it) {
        const qint32 pid = it.key();
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_FILEDESC, pid};
        const std::vector<char> buf = suckMib(mib, 4);
        if (buf.empty())
            continue;
        const char *p = buf.data();
        const char *end = p + buf.size();
        while (p + static_cast<long>(sizeof(int)) <= end) {
            const auto *kf = reinterpret_cast<const struct kinfo_file *>(p);
            const int sz = kf->kf_structsize;
            if (sz <= 0)
                break;
            const char *next = p + sz;
            p = next;

            if (kf->kf_type != KF_TYPE_SOCKET)
                continue;
            const int dom = kf->kf_sock_domain;
            if (dom != AF_INET && dom != AF_INET6)
                continue;
            L4Proto proto;
            if (kf->kf_sock_protocol == IPPROTO_TCP)      proto = L4Proto::Tcp;
            else if (kf->kf_sock_protocol == IPPROTO_UDP) proto = L4Proto::Udp;
            else                                          continue;

            const Endpoint local  = endpointFromSockaddr(
                reinterpret_cast<const sockaddr *>(&kf->kf_sa_local));
            const Endpoint remote = endpointFromSockaddr(
                reinterpret_cast<const sockaddr *>(&kf->kf_sa_peer));
            if (local.address.isNull())
                continue;

            const ProcessInfo &info = it.value();
            if (!remote.address.isNull() && remote.port != 0)
                m_exact.insert(flowKeyExact(proto, local, remote), info);
            const QString lk = flowKeyLocal(proto, local);
            if (!m_local.contains(lk))
                m_local.insert(lk, info);
        }
    }
}

#else // other BSDs: attribution not implemented (capture still works)

void BsdSocketResolver::refresh() {}

#endif

} // namespace qiftop::backend::bsd
