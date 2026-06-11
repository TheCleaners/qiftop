#pragma once

#include "backend/Connection.h"

#include <QString>

namespace qiftop::backend::bsd {

// Canonical flow keys shared by the capture worker and the socket→process
// resolver so a captured flow and its owning PCB hash identically.
//   exact:  "proto|laddr.lport|raddr.rport"
//   local:  "proto|laddr.lport"  (fallback for listeners / unconnected UDP,
//                                  mirroring the Linux sock_diag 2-tuple path)
inline QString flowKeyExact(L4Proto proto, const Endpoint &local,
                            const Endpoint &remote)
{
    return QStringLiteral("%1|%2.%3|%4.%5")
        .arg(static_cast<int>(proto))
        .arg(local.address.toString()).arg(local.port)
        .arg(remote.address.toString()).arg(remote.port);
}

inline QString flowKeyLocal(L4Proto proto, const Endpoint &local)
{
    return QStringLiteral("%1|%2.%3")
        .arg(static_cast<int>(proto))
        .arg(local.address.toString()).arg(local.port);
}

} // namespace qiftop::backend::bsd
