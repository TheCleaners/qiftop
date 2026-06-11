#pragma once

#include "backend/Connection.h"

#include <QHostAddress>
#include <QSet>

namespace qiftop::backend::linux {

[[nodiscard]] inline Connection orientConntrackFlow(const QHostAddress &src,
                                                    const QHostAddress &dst,
                                                    quint16 sport,
                                                    quint16 dport,
                                                    L4Proto proto,
                                                    quint64 origBytes,
                                                    quint64 replBytes,
                                                    quint64 origPackets,
                                                    quint64 replPackets,
                                                    const QSet<QHostAddress> &localAddrs)
{
    Connection c;
    c.proto = proto;

    const bool srcLocal = localAddrs.contains(src);
    const bool dstLocal = localAddrs.contains(dst);
    if (srcLocal || !dstLocal) {
        c.local     = {src, sport};
        c.remote    = {dst, dport};
        c.txBytes   = origBytes;
        c.rxBytes   = replBytes;
        c.txPackets = origPackets;
        c.rxPackets = replPackets;
    } else {
        c.local     = {dst, dport};
        c.remote    = {src, sport};
        c.txBytes   = replBytes;
        c.rxBytes   = origBytes;
        c.txPackets = replPackets;
        c.rxPackets = origPackets;
    }

    return c;
}

} // namespace qiftop::backend::linux
