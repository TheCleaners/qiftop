#include <cstring>

#include <linux/types.h>

#include <QHostAddress>
#include <QtTest>

#include "backend/linux/BirthDecode.h"
#include "backend/linux/bpf/birth_events.h"

using namespace qiftop::backend;
using namespace qiftop::backend::linuximpl;

// Pure decode test for the eBPF birth wire event → (BirthKey, BirthRecord). No
// kernel, no libbpf — events are hand-built. Pins the byte-order/units contract
// the BpfBirthReader relies on (and that the PID-reuse guard depends on).
class TestBirthDecode : public QObject
{
    Q_OBJECT

    static qiftop_birth_event v4Tcp()
    {
        qiftop_birth_event e{};
        e.family    = AF_INET;
        e.proto     = 6; // IPPROTO_TCP
        e.direction = QIFTOP_BIRTH_DIR_OUTBOUND;
        // 192.168.1.10 → :54321  ->  93.184.216.34 : 443
        e.local_addr[0] = 192; e.local_addr[1] = 168; e.local_addr[2] = 1;  e.local_addr[3] = 10;
        e.remote_addr[0] = 93; e.remote_addr[1] = 184; e.remote_addr[2] = 216; e.remote_addr[3] = 34;
        e.local_port  = 54321;
        e.remote_port = 443;
        e.pid = 4242;
        e.uid = 1000;
        std::memcpy(e.comm, "curl", 4);
        e.start_boottime_ns = 12'300'000'000ULL; // *100/1e9 = 1230 ticks
        e.ts_ns             = 5'000'000'000ULL;  // /1e6     = 5000 ms
        return e;
    }

private slots:
    void v4OutboundTcp()
    {
        const DecodedBirth d = decodeBirth(v4Tcp(), /*clkTck*/ 100, /*nowMonoMs*/ 999);

        QCOMPARE(d.key.proto, L4Proto::Tcp);
        QCOMPARE(d.key.localAddress.toString(), QStringLiteral("192.168.1.10"));
        QCOMPARE(d.key.remoteAddress.toString(), QStringLiteral("93.184.216.34"));
        QCOMPARE(d.key.localPort, quint16(54321));
        QCOMPARE(d.key.remotePort, quint16(443));

        QCOMPARE(d.rec.pid, qint32(4242));
        QCOMPARE(d.rec.uid, quint32(1000));
        QCOMPARE(d.rec.comm, QStringLiteral("curl"));
        QCOMPARE(d.rec.direction, Direction::Outbound);
        QCOMPARE(d.rec.startTime, quint64(1230)); // ns→ticks via USER_HZ=100
        QCOMPARE(d.rec.firstSeenMonoMs, qint64(5000));
        QCOMPARE(d.rec.insertedMonoMs, qint64(999));
    }

    void startTimeUsesClkTck()
    {
        // The conversion must track sysconf(_SC_CLK_TCK), not assume 100.
        qiftop_birth_event e = v4Tcp();
        e.start_boottime_ns = 1'000'000'000ULL; // exactly 1 s of boottime
        QCOMPARE(decodeBirth(e, 100,  0).rec.startTime, quint64(100));
        QCOMPARE(decodeBirth(e, 250,  0).rec.startTime, quint64(250));
        QCOMPARE(decodeBirth(e, 1000, 0).rec.startTime, quint64(1000));
        // Degenerate clkTck → 0 (guard then never false-rejects on starttime).
        QCOMPARE(decodeBirth(e, 0, 0).rec.startTime, quint64(0));
    }

    void v6InboundTcp()
    {
        qiftop_birth_event e{};
        e.family    = AF_INET6;
        e.proto     = 6;
        e.direction = QIFTOP_BIRTH_DIR_INBOUND;
        // 2001:db8::1  ->  2001:db8::2
        e.local_addr[0]  = 0x20; e.local_addr[1]  = 0x01; e.local_addr[2]  = 0x0d; e.local_addr[3] = 0xb8;
        e.local_addr[15] = 0x01;
        e.remote_addr[0] = 0x20; e.remote_addr[1] = 0x01; e.remote_addr[2] = 0x0d; e.remote_addr[3] = 0xb8;
        e.remote_addr[15] = 0x02;
        e.local_port  = 8443;
        e.remote_port = 33000;
        e.pid = 7;
        std::memcpy(e.comm, "nginx", 5);

        const DecodedBirth d = decodeBirth(e, 100, 0);
        QCOMPARE(d.key.localAddress, QHostAddress(QStringLiteral("2001:db8::1")));
        QCOMPARE(d.key.remoteAddress, QHostAddress(QStringLiteral("2001:db8::2")));
        QCOMPARE(d.key.localPort, quint16(8443));
        QCOMPARE(d.rec.direction, Direction::Inbound);
        QCOMPARE(d.rec.comm, QStringLiteral("nginx"));
    }

    void udpProtoAndUnknownDirection()
    {
        qiftop_birth_event e = v4Tcp();
        e.proto     = 17; // IPPROTO_UDP
        e.direction = QIFTOP_BIRTH_DIR_UNKNOWN;
        const DecodedBirth d = decodeBirth(e, 100, 0);
        QCOMPARE(d.key.proto, L4Proto::Udp);
        QCOMPARE(d.rec.direction, Direction::Unknown);
    }

    void commIsNulBoundedAndNeverOverreads()
    {
        // A comm with no NUL terminator in all 16 bytes must read at most 16.
        qiftop_birth_event e = v4Tcp();
        std::memset(e.comm, 'a', QIFTOP_BIRTH_COMM_LEN);
        const DecodedBirth d = decodeBirth(e, 100, 0);
        QCOMPARE(d.rec.comm.size(), QIFTOP_BIRTH_COMM_LEN);
        QVERIFY(std::all_of(d.rec.comm.begin(), d.rec.comm.end(),
                            [](QChar c) { return c == QLatin1Char('a'); }));
    }
};

QTEST_APPLESS_MAIN(TestBirthDecode)
#include "test_birth_decode.moc"
