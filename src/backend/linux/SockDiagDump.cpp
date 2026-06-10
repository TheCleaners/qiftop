#include "SockDiagDump.h"

#include <QAtomicInt>
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

// L3: these helpers run inside per-netns loops (up to 256 namespaces ×
// 4 dumps per tick, every 5 s). A steady-state failure (kernel without
// sock_diag, sandbox blocking netlink) would otherwise log per call
// and drown the journal. Warn ONCE per process lifetime per failure
// site — the conditions are steady-state, not per-tick races.
bool warnOnce(QAtomicInt &flag)
{
    return flag.testAndSetRelaxed(0, 1);
}

QAtomicInt g_warnedSocket  = 0;
QAtomicInt g_warnedBind    = 0;
QAtomicInt g_warnedSend    = 0;
QAtomicInt g_warnedRecv    = 0;
QAtomicInt g_warnedNlError = 0;
QAtomicInt g_warnedCap     = 0;

} // namespace

int openSockDiagSocket()
{
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                            NETLINK_SOCK_DIAG);
    if (fd < 0) {
        if (warnOnce(g_warnedSocket)) {
            qCWarning(lcVerbose) << "sockdiag::openSockDiagSocket: socket:"
                                 << std::strerror(errno)
                                 << "(warning once; suppressing repeats)";
        }
        return -1;
    }
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        if (warnOnce(g_warnedBind)) {
            qCWarning(lcVerbose) << "sockdiag::openSockDiagSocket: bind:"
                                 << std::strerror(errno)
                                 << "(warning once; suppressing repeats)";
        }
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
        if (warnOnce(g_warnedSend)) {
            qCWarning(lcVerbose) << "sockdiag::dumpSocketsViaFd: send:"
                                 << std::strerror(errno)
                                 << "(warning once; suppressing repeats)";
        }
        return false;
    }

    std::array<char, 16384> buf{};
    for (;;) {
        ssize_t n = ::recv(nlFd, buf.data(), buf.size(), MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (warnOnce(g_warnedRecv)) {
                qCWarning(lcVerbose) << "sockdiag::dumpSocketsViaFd: recv:"
                                     << std::strerror(errno)
                                     << "(warning once; suppressing repeats)";
            }
            return false;
        }
        // MSG_TRUNC makes recv() report the REAL datagram length even
        // when it exceeded our buffer — in that case the tail was
        // discarded by the kernel and parsing the partial buffer would
        // read decapitated messages. Bail on the whole dump.
        if (n > static_cast<ssize_t>(buf.size())) {
            static QAtomicInt warnedOnce = 0;
            if (warnedOnce.testAndSetRelaxed(0, 1)) {
                qWarning() << "sockdiag::dumpSocketsViaFd: oversized netlink message"
                           << n << "bytes exceeds buffer" << buf.size()
                           << "bytes; skipping dump";
            }
            return false;
        }
        if (n == 0) return true;

        int nlErrno = 0;
        switch (parseDumpChunk(buf.data(), n, proto, outMap, &nlErrno)) {
        case DumpChunkResult::Done:
            if (outMap.size() >= kMaxSocketEntries
                && warnOnce(g_warnedCap)) {
                qWarning("sockdiag: socket table hit the %d-entry cap; "
                         "flows beyond it stay unattributed this tick "
                         "(warning once; suppressing repeats)",
                         kMaxSocketEntries);
            }
            return true;
        case DumpChunkResult::Failed:
            if (warnOnce(g_warnedNlError)) {
                qCWarning(lcVerbose)
                    << "sockdiag::dumpSocketsViaFd: NLMSG_ERROR/malformed:"
                    << (nlErrno ? std::strerror(nlErrno) : "short message")
                    << "(warning once; suppressing repeats)";
            }
            return false;
        case DumpChunkResult::NeedMore:
            break;
        }
    }
}

} // namespace qiftop::backend::sockdiag

