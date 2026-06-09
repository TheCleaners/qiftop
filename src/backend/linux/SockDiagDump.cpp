#include "SockDiagDump.h"

#include <QtEndian>

#include <array>
#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>

#include "util/Logging.h"

namespace qiftop::backend::sockdiag {

namespace {

QHostAddress addrFromDiag(quint8 family, const __be32 buf[4])
{
    if (family == AF_INET) {
        return QHostAddress(qFromBigEndian<quint32>(buf[0]));
    }
    Q_IPV6ADDR v6;
    std::memcpy(&v6, buf, sizeof(v6));
    return QHostAddress(v6);
}

} // namespace

QByteArray makeFlowKey(quint8 proto,
                       const QHostAddress &localAddr,  quint16 localPort,
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
    append(localAddr,  localPort);
    append(remoteAddr, remotePort);
    return k;
}

int openSockDiagSocket()
{
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                            NETLINK_SOCK_DIAG);
    if (fd < 0) {
        qCWarning(lcVerbose) << "sockdiag::openSockDiagSocket: socket:"
                             << std::strerror(errno);
        return -1;
    }
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        qCWarning(lcVerbose) << "sockdiag::openSockDiagSocket: bind:"
                             << std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

bool dumpSocketsViaFd(int nlFd, quint8 family, quint8 proto,
                      QHash<QByteArray, quint64> &outMap, quint32 seqHint)
{
    struct {
        nlmsghdr           nlh;
        inet_diag_req_v2   req;
    } msg{};
    msg.nlh.nlmsg_len   = sizeof(msg);
    msg.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.nlh.nlmsg_seq   = seqHint;
    msg.req.sdiag_family   = family;
    msg.req.sdiag_protocol = proto;
    // TCP: every state; UDP: only one "state" but the kernel requires
    // the mask to be non-zero. ~0 is the ss(8) idiom.
    msg.req.idiag_states   = ~0u;
    msg.req.idiag_ext      = 0;
    msg.req.id             = {};

    if (::send(nlFd, &msg, sizeof(msg), 0) < 0) {
        qCWarning(lcVerbose) << "sockdiag::dumpSocketsViaFd: send:"
                             << std::strerror(errno);
        return false;
    }

    std::array<char, 16384> buf{};
    for (;;) {
        ssize_t n = ::recv(nlFd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            qCWarning(lcVerbose) << "sockdiag::dumpSocketsViaFd: recv:"
                                 << std::strerror(errno);
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
                    qCWarning(lcVerbose) << "sockdiag::dumpSocketsViaFd: NLMSG_ERROR:"
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
            outMap.insert(makeFlowKey(proto, local, lport, remote, rport),
                          m->idiag_inode);
        }
    }
}

} // namespace qiftop::backend::sockdiag
