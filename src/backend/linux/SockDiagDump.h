#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>

namespace qiftop::backend::sockdiag {

// Compose the 4-tuple lookup key used across the resolver layer. proto
// is the IANA L4 number (TCP=6, UDP=17). The byte order and field
// order MUST stay stable — any change is a silent cache-miss event for
// every in-process resolver. Exposed here so NetnsScanner / future
// resolvers produce IDENTICAL keys to SockDiagResolver.
[[nodiscard]] QByteArray makeFlowKey(quint8 proto,
                                     const QHostAddress &localAddr,
                                     quint16            localPort,
                                     const QHostAddress &remoteAddr,
                                     quint16            remotePort);

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

} // namespace qiftop::backend::sockdiag
