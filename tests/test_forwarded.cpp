// Forwarded-flow detection. Pure.

#include <QHostAddress>
#include <QSet>
#include <QtTest/QtTest>

#include "backend/Connection.h"
#include "util/ConnectionHeuristics.h"

using qiftop::heuristics::isForwardedFlow;
using qiftop::heuristics::attributionReason;

namespace {
Connection mk(const char *l, const char *r)
{
    Connection c;
    c.proto         = L4Proto::Udp;
    c.local.address = QHostAddress(QString::fromLatin1(l));
    c.remote.address = QHostAddress(QString::fromLatin1(r));
    return c;
}
QSet<QHostAddress> loopback()
{
    return { QHostAddress(QHostAddress::LocalHost),
             QHostAddress(QHostAddress::LocalHostIPv6) };
}
} // namespace

class TestForwarded : public QObject {
    Q_OBJECT
private slots:
    void neither_local_is_forwarded()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        QVERIFY(isForwardedFlow(mk("10.42.0.158", "108.61.73.244"), local, loopback()));
    }

    void local_src_is_not_forwarded()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        QVERIFY(!isForwardedFlow(mk("192.168.1.10", "8.8.8.8"), local, loopback()));
    }

    void local_dst_is_not_forwarded()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        QVERIFY(!isForwardedFlow(mk("8.8.8.8", "192.168.1.10"), local, loopback()));
    }

    void loopback_counts_as_local()
    {
        QVERIFY(!isForwardedFlow(mk("127.0.0.1", "127.0.0.1"), {}, loopback()));
        QVERIFY(!isForwardedFlow(mk("::1",       "::1"      ), {}, loopback()));
    }

    void empty_local_set_makes_almost_everything_forwarded()
    {
        // With no known local addrs, anything non-loopback is forwarded.
        QVERIFY(isForwardedFlow(mk("8.8.8.8", "1.1.1.1"), {}, loopback()));
        QVERIFY(!isForwardedFlow(mk("127.0.0.1", "8.8.8.8"), {}, loopback()));
    }

    // --- attributionReason -------------------------------------------------

    void reason_resolved_when_pid_present()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        Connection c = mk("192.168.1.10", "8.8.8.8");
        c.process.pid = 4242;
        QCOMPARE(attributionReason(c, local, loopback()),
                 AttributionReason::Resolved);
    }

    void reason_forwarded_when_neither_end_local()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        // pid==0, neither end is ours → forwarded (router/NAT traffic).
        Connection c = mk("10.42.0.122", "84.17.53.155");
        QCOMPARE(attributionReason(c, local, loopback()),
                 AttributionReason::Forwarded);
    }

    void reason_orphaned_for_tcp_teardown()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        Connection c = mk("192.168.1.10", "1.1.1.1");
        c.proto    = L4Proto::Tcp;
        c.tcpState = TcpState::TimeWait;     // socket gone
        QCOMPARE(attributionReason(c, local, loopback()),
                 AttributionReason::Orphaned);
    }

    void reason_nolocalsocket_for_local_flow_without_socket()
    {
        QSet<QHostAddress> local{ QHostAddress("192.168.1.10") };
        // Local UDP flow, pid==0, not forwarded, not a TCP teardown:
        // a closed UDP conntrack husk / kernel socket / a miss.
        Connection c = mk("192.168.1.10", "75.75.75.75");
        QCOMPARE(attributionReason(c, local, loopback()),
                 AttributionReason::NoLocalSocket);
    }

    void reason_resolved_wins_even_when_forwarded_shaped()
    {
        // A PID always wins: even neither-end-local can't override an
        // actual attribution (e.g. a netns-scanned container flow).
        Connection c = mk("10.42.0.122", "84.17.53.155");
        c.process.pid = 99;
        QCOMPARE(attributionReason(c, {}, loopback()),
                 AttributionReason::Resolved);
    }
};

QTEST_APPLESS_MAIN(TestForwarded)
#include "test_forwarded.moc"
