#pragma once

// Pure decoder: one eBPF wire birth event → (BirthKey, BirthRecord). No kernel,
// no /proc, no libbpf — just struct reshaping + byte-order/units conversion, so
// it's unit-tested without loading any BPF. The ring-buffer reader
// (BpfBirthReader) calls this on every drained event; tests call it with
// hand-built events.
//
// NOT gated on QIFTOP_HAVE_BPF: it needs only the shared wire header, not the
// generated skeleton, so the decode logic is always built and tested even on
// images without the eBPF toolchain.

#include <cstring>

#include <linux/types.h>   // __u8/__u16/__u32/__u64 for the wire header
#include <sys/socket.h>    // AF_INET6

#include <QHostAddress>
#include <QString>
#include <QtEndian>

#include "backend/BirthCache.h"
#include "backend/Connection.h"
#include "backend/linux/bpf/birth_events.h"

namespace qiftop::backend::linuximpl {

struct DecodedBirth {
    BirthKey    key;
    BirthRecord rec;
};

// Decode one wire event.
//   clkTck    — sysconf(_SC_CLK_TCK): ticks/sec, to convert start_boottime_ns
//               into the /proc field-22 clock-tick units the PID-reuse guard
//               compares against (kernel: nsec_to_clock_t = ns * USER_HZ / 1e9).
//   nowMonoMs — CLOCK_MONOTONIC ms at insertion (cache TTL aging base).
[[nodiscard]] inline DecodedBirth
decodeBirth(const qiftop_birth_event &e, long clkTck, qint64 nowMonoMs)
{
    DecodedBirth d;

    d.key.proto      = fromIanaProto(e.proto);
    d.key.localPort  = e.local_port;   // already host order on the wire
    d.key.remotePort = e.remote_port;

    if (e.family == AF_INET6) {
        Q_IPV6ADDR a6;
        std::memcpy(a6.c, e.local_addr, 16);
        d.key.localAddress = QHostAddress(a6);
        std::memcpy(a6.c, e.remote_addr, 16);
        d.key.remoteAddress = QHostAddress(a6);
    } else {
        // local_addr[0..4) hold the __be32 in network order; QHostAddress(quint32)
        // wants host order, so read the bytes big-endian.
        d.key.localAddress  = QHostAddress(qFromBigEndian<quint32>(e.local_addr));
        d.key.remoteAddress = QHostAddress(qFromBigEndian<quint32>(e.remote_addr));
    }

    d.rec.pid = static_cast<qint32>(e.pid);
    d.rec.uid = e.uid;

    const auto *comm = reinterpret_cast<const char *>(e.comm);
    d.rec.comm = QString::fromUtf8(comm,
        static_cast<int>(::strnlen(comm, QIFTOP_BIRTH_COMM_LEN)));

    d.rec.direction = (e.direction == QIFTOP_BIRTH_DIR_OUTBOUND) ? Direction::Outbound
                    : (e.direction == QIFTOP_BIRTH_DIR_INBOUND)  ? Direction::Inbound
                                                                 : Direction::Unknown;

    d.rec.startTime = (clkTck > 0)
        ? (e.start_boottime_ns * static_cast<quint64>(clkTck) / 1'000'000'000ULL)
        : 0;
    d.rec.firstSeenMonoMs = static_cast<qint64>(e.ts_ns / 1'000'000ULL);
    d.rec.insertedMonoMs  = nowMonoMs;
    return d;
}

} // namespace qiftop::backend::linuximpl
