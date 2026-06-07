// Forwarded-flow detection. Pure.

#include <QHostAddress>
#include <QSet>
#include <QtTest/QtTest>

#include "backend/Connection.h"
#include "ui/ConnectionHeuristics.h"

using qiftop::heuristics::isForwardedFlow;

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
};

QTEST_APPLESS_MAIN(TestForwarded)
#include "test_forwarded.moc"
