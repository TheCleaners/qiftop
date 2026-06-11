#include "BsdSocketResolver.h"
#include "BsdFlowKey.h"

#include "util/Logging.h"

#if defined(__NetBSD__)
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/file.h>

#include <netinet/in.h>

#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <QtEndian>
#endif

namespace qiftop::backend::bsd {

#if defined(__NetBSD__)

namespace {

// Fetch a variable-size sysctl array, growing the buffer until it fits.
// `name`/`namelen` is the fully-built MIB including the trailing
// (op, arg, elemsize, count) elements the KERN_*2 / pcblist nodes require.
//
// Note: a sysctl size-probe (oldp == NULL) returns 0 and reports the needed
// size in `sz` — it does NOT fail with ENOMEM. So we must keep iterating
// after the probe (allocate, then fetch), and only stop once we've actually
// read into a non-null buffer. Mirrors sockstat's sysctl_sucker.
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
        if (rc == 0 && old != nullptr)
            break;               // fetched real data
        buf.resize(sz);          // size probe (rc==0, old==NULL) or ENOMEM grow
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

// socket kernel pointer -> owning pid via KERN_FILE2 / KERN_FILE_BYPID
// (BYPID populates ki_pid per descriptor, which BYFILE does not).
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
            continue; // node absent (e.g. IPv6 disabled)
        u_int namelen = static_cast<u_int>(depth);
        name[namelen++] = PCB_ALL;
        name[namelen++] = 0;                          // all pids
        name[namelen++] = sizeof(struct kinfo_pcb);
        name[namelen++] = INT_MAX;                    // all entries

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

#else // !__NetBSD__

void BsdSocketResolver::refresh() {}
ProcessInfo BsdSocketResolver::lookup(L4Proto, const Endpoint &, const Endpoint &) const
{
    return {};
}

#endif

} // namespace qiftop::backend::bsd
