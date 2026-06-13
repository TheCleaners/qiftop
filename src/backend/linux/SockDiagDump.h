#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QtEndian>

#include <cstring>

#include <sys/socket.h>

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>

namespace qiftop::backend::sockdiag {

// M9: hard cap on every socket→inode / inode→pid map built by the
// resolver layer (host dump, per-netns dumps, /proc fd walks). A
// hostile or pathological workload can hold millions of sockets; an
// unbounded map would stall the dump loop and OOM the root agent.
// Mirrors the CgroupClassifier::kCacheMaxItems bounded-cache rule
// (AGENTS.md §8a rule 8) — these maps are rebuilt from scratch every
// refresh tick, so the bounded analogue of clear-on-overflow is
// "stop enrolling at the cap": attribution stays intact for the
// first kMaxSocketEntries sockets and the tick completes in bounded
// time/memory. Generous: a busy host has a few thousand sockets.
inline constexpr int kMaxSocketEntries = 65536;
inline constexpr quint64 kAmbiguousLocalInode = 0;

// Compose the 4-tuple lookup key used across the resolver layer. proto
// is the IANA L4 number (TCP=6, UDP=17). The byte order and field
// order MUST stay stable — any change is a silent cache-miss event for
// every in-process resolver. Exposed here so NetnsScanner / future
// resolvers produce IDENTICAL keys to SockDiagResolver. Header-inline
// so the pure parser below (and its unit test) need no extra TU.
[[nodiscard]] inline QByteArray makeFlowKey(quint8 proto,
                                            const QHostAddress &localAddr,
                                            quint16            localPort,
                                            const QHostAddress &remoteAddr,
                                            quint16            remotePort)
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

// Local-only ("2-tuple") key for matching flows to sockets that carry no
// remote in sock_diag: unconnected UDP sockets (the common case — a UDP
// service binds a local addr/port and recvfrom()s from many peers) and
// listening TCP sockets. The full 4-tuple key above never matches those
// because their idiag_dst is 0.0.0.0:0 while a live conntrack flow has a real
// remote. A distinct 1-byte tag prevents any collision with a 4-tuple key.
[[nodiscard]] inline QByteArray makeLocalKey(quint8 proto,
                                             const QHostAddress &localAddr,
                                             quint16            localPort)
{
    QByteArray k;
    k.reserve(24);
    k.append('\x01');                       // tag: local-only key namespace
    k.append(static_cast<char>(proto));
    if (localAddr.protocol() == QAbstractSocket::IPv6Protocol) {
        Q_IPV6ADDR v6 = localAddr.toIPv6Address();
        k.append(reinterpret_cast<const char*>(&v6), sizeof(v6));
    } else {
        quint32 v4 = qToBigEndian(localAddr.toIPv4Address());
        k.append(reinterpret_cast<const char*>(&v4), sizeof(v4));
    }
    quint16 p = qToBigEndian(localPort);
    k.append(reinterpret_cast<const char*>(&p), sizeof(p));
    return k;
}

[[nodiscard]] inline bool isLocalKey(const QByteArray &key)
{
    return !key.isEmpty() && key.at(0) == '\x01';
}

[[nodiscard]] inline bool isAmbiguousLocalInode(quint64 inode)
{
    return inode == kAmbiguousLocalInode;
}

inline void insertLocalKey(QHash<QByteArray, quint64> &outMap,
                           const QByteArray           &localKey,
                           quint64                     inode)
{
    auto it = outMap.find(localKey);
    if (it == outMap.end()) {
        if (outMap.size() < kMaxSocketEntries)
            outMap.insert(localKey, inode);
        return;
    }
    if (it.value() != inode) {
        it.value() = kAmbiguousLocalInode;
    }
}

// Open + bind a NETLINK_SOCK_DIAG socket in the calling thread's
// CURRENT network namespace. Returns the fd on success, -1 on failure
// (with a single qCWarning). Caller closes via ::close. Must be opened
// AFTER any setns(2) call — netlink sockets are bound to the netns
// they were created in for life.
[[nodiscard]] int openSockDiagSocket();

// Issue one sock_diag dump on an already-opened netlink fd for the
// given (family, proto) pair, populating `outMap[4-tuple-key] = inode`.
// `seqHint` is used as nlmsg_seq for the dump request — any nonzero
// value works; we just take it so concurrent dumps on separate fds
// don't share a seq.
//
// Returns true on a successful dump (including the empty case),
// false on netlink error.
[[nodiscard]] bool dumpSocketsViaFd(int                                 nlFd,
                                    quint8                              family,
                                    quint8                              proto,
                                    QHash<QByteArray, quint64>         &outMap,
                                    quint32                             seqHint);

// ---------------------------------------------------------------------------
// Pure buffer-level parsing of one recv()'d chunk of a sock_diag dump.
// Header-inline so unit tests can drive it with synthetic netlink
// buffers — no socket, no kernel (tests/test_sockdiag_parse.cpp).
// ---------------------------------------------------------------------------

enum class DumpChunkResult {
    NeedMore,   // chunk fully consumed; the dump continues
    Done,       // NLMSG_DONE (or an ACK NLMSG_ERROR) seen — dump complete
    Failed,     // NLMSG_ERROR with a nonzero error, or malformed message
};

namespace detail {
inline QHostAddress addrFromDiag(quint8 family, const __be32 buf[4])
{
    if (family == AF_INET) {
        return QHostAddress(qFromBigEndian<quint32>(buf[0]));
    }
    Q_IPV6ADDR v6;
    std::memcpy(&v6, buf, sizeof(v6));
    return QHostAddress(v6);
}
} // namespace detail

// Walk the netlink messages in `buf[0..n)`, inserting flow-key → inode
// entries into outMap (capped at kMaxSocketEntries — see above). Local
// 2-tuple entries are ambiguity-aware: a key seen for multiple distinct
// socket inodes is stored as kAmbiguousLocalInode and must not attribute.
// On
// Failed, *nlErrno (when non-null) receives the positive errno carried
// by a well-formed NLMSG_ERROR, or 0 for a malformed/truncated message.
//
// M10: every header- and payload-length is validated BEFORE the
// payload is dereferenced. NLMSG_OK only guarantees the message fits
// the buffer and is >= sizeof(nlmsghdr) — an NLMSG_ERROR whose
// nlmsg_len is shorter than NLMSG_LENGTH(sizeof(nlmsgerr)) would
// otherwise be an over-read off the end of a short datagram.
inline DumpChunkResult parseDumpChunk(const char *buf, qsizetype n,
                                      quint8 proto,
                                      QHash<QByteArray, quint64> &outMap,
                                      int *nlErrno = nullptr)
{
    if (nlErrno) *nlErrno = 0;
    auto remaining = static_cast<int>(n);
    for (const auto *nh = reinterpret_cast<const nlmsghdr*>(buf);
         NLMSG_OK(nh, remaining);
         nh = NLMSG_NEXT(nh, remaining)) {
        if (nh->nlmsg_type == NLMSG_DONE) return DumpChunkResult::Done;
        if (nh->nlmsg_type == NLMSG_ERROR) {
            // Validate the payload really holds a struct nlmsgerr
            // before touching it (M10).
            if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
                return DumpChunkResult::Failed;
            }
            const auto *err = static_cast<const nlmsgerr*>(
                NLMSG_DATA(const_cast<nlmsghdr*>(nh)));
            if (err->error != 0) {
                if (nlErrno) *nlErrno = -err->error;
                return DumpChunkResult::Failed;
            }
            return DumpChunkResult::Done;   // ACK
        }
        if (nh->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;
        if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(inet_diag_msg))) continue;
        const auto *m = static_cast<const inet_diag_msg*>(
            NLMSG_DATA(const_cast<nlmsghdr*>(nh)));
        if (m->idiag_inode == 0) continue;
        const auto local  = detail::addrFromDiag(m->idiag_family, m->id.idiag_src);
        const auto remote = detail::addrFromDiag(m->idiag_family, m->id.idiag_dst);
        const quint16 lport = qFromBigEndian(m->id.idiag_sport);
        const quint16 rport = qFromBigEndian(m->id.idiag_dport);
        const quint64 inode = m->idiag_inode;
        const auto flowKey = makeFlowKey(proto, local, lport, remote, rport);
        if (outMap.size() >= kMaxSocketEntries && !outMap.contains(flowKey)) continue;   // M9 cap
        outMap.insert(flowKey, inode);
        // Also index by local-only key so unconnected UDP sockets / listeners
        // (whose remote is 0.0.0.0:0) can be matched to a live flow by its
        // local end. Multiple distinct inodes for the same local endpoint are
        // ambiguous (SO_REUSEPORT / wildcard+specific binds), so mark them and
        // let the resolver return pid=0 rather than a last-writer-wins pid.
        insertLocalKey(outMap, makeLocalKey(proto, local, lport), inode);

        // Dual-stack (AF_INET6) sockets serving IPv4 peers appear in this v6
        // dump with v4-mapped (::ffff:a.b.c.d) or v6-wildcard (::) addresses,
        // but conntrack/pcap report those same flows as PURE IPv4. Without an
        // IPv4-normalised key the v4 flow can never join the v6 socket, so
        // EVERY dual-stack daemon's v4 traffic (kdeconnect, sshd, most JVM and
        // many server processes) reports pid=0. Re-index such sockets under
        // IPv4 keys too. Genuine IPv6 addresses (global, ::1) return
        // toIPv4Address ok=false and are left untouched. This mirrors how the
        // kernel/ss(8) attribute v4 flows to v6-mapped sockets.
        if (m->idiag_family == AF_INET6) {
            bool lok = false, rok = false;
            const QHostAddress l4(local.toIPv4Address(&lok));
            const QHostAddress r4(remote.toIPv4Address(&rok));
            if (lok) {
                if (rok) {
                    const auto k4 = makeFlowKey(proto, l4, lport, r4, rport);
                    if (!(outMap.size() >= kMaxSocketEntries
                          && !outMap.contains(k4)))
                        outMap.insert(k4, inode);
                }
                insertLocalKey(outMap, makeLocalKey(proto, l4, lport), inode);
            }
        }
    }
    return DumpChunkResult::NeedMore;
}

} // namespace qiftop::backend::sockdiag
