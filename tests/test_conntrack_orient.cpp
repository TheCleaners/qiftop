#include <QHostAddress>
#include <QSet>
#include <QtTest/QtTest>

#include "backend/linux/ConntrackOrient.h"

using qiftop::backend::linux::orientConntrackFlow;

namespace {

constexpr quint64 kOrigBytes = 1000;
constexpr quint64 kReplBytes = 2000;
constexpr quint64 kOrigPkts  = 10;
constexpr quint64 kReplPkts  = 20;

Connection orient(const char *src, quint16 sport,
                  const char *dst, quint16 dport,
                  const QSet<QHostAddress> &localAddrs)
{
    return orientConntrackFlow(QHostAddress(QString::fromLatin1(src)),
                               QHostAddress(QString::fromLatin1(dst)),
                               sport, dport, L4Proto::Tcp,
                               kOrigBytes, kReplBytes,
                               kOrigPkts, kReplPkts,
                               localAddrs);
}

void QCOMPARE_ENDPOINT(const Endpoint &endpoint, const char *address, quint16 port)
{
    QCOMPARE(endpoint.address, QHostAddress(QString::fromLatin1(address)));
    QCOMPARE(endpoint.port, port);
}

void QCOMPARE_COUNTERS(const Connection &c,
                       quint64 txBytes, quint64 rxBytes,
                       quint64 txPackets, quint64 rxPackets)
{
    QCOMPARE(c.txBytes, txBytes);
    QCOMPARE(c.rxBytes, rxBytes);
    QCOMPARE(c.txPackets, txPackets);
    QCOMPARE(c.rxPackets, rxPackets);
}

} // namespace

class TestConntrackOrient : public QObject {
    Q_OBJECT

private slots:
    void outboundSrcLocalUsesOrigAsTx()
    {
        const QSet<QHostAddress> local{QHostAddress(QStringLiteral("192.168.1.10"))};

        const Connection c = orient("192.168.1.10", 51234,
                                    "8.8.8.8", 443,
                                    local);

        QCOMPARE(c.proto, L4Proto::Tcp);
        QCOMPARE_ENDPOINT(c.local, "192.168.1.10", 51234);
        QCOMPARE_ENDPOINT(c.remote, "8.8.8.8", 443);
        QCOMPARE_COUNTERS(c, kOrigBytes, kReplBytes, kOrigPkts, kReplPkts);
    }

    void inboundDstLocalSwapsTupleAndCounters()
    {
        const QSet<QHostAddress> local{QHostAddress(QStringLiteral("192.168.1.10"))};

        const Connection c = orient("203.0.113.8", 51515,
                                    "192.168.1.10", 22,
                                    local);

        QCOMPARE_ENDPOINT(c.local, "192.168.1.10", 22);
        QCOMPARE_ENDPOINT(c.remote, "203.0.113.8", 51515);
        QCOMPARE_COUNTERS(c, kReplBytes, kOrigBytes, kReplPkts, kOrigPkts);
    }

    void forwardedNeitherLocalKeepsOrigTuple()
    {
        const QSet<QHostAddress> local{QHostAddress(QStringLiteral("192.168.1.10"))};

        const Connection c = orient("10.0.0.5", 12345,
                                    "198.51.100.9", 443,
                                    local);

        QCOMPARE_ENDPOINT(c.local, "10.0.0.5", 12345);
        QCOMPARE_ENDPOINT(c.remote, "198.51.100.9", 443);
        QCOMPARE_COUNTERS(c, kOrigBytes, kReplBytes, kOrigPkts, kReplPkts);
    }

    void bothLocalSrcLocalRuleWins()
    {
        const QSet<QHostAddress> local{
            QHostAddress(QStringLiteral("192.168.1.10")),
            QHostAddress(QStringLiteral("192.168.1.11")),
        };

        const Connection c = orient("192.168.1.10", 40000,
                                    "192.168.1.11", 8080,
                                    local);

        QCOMPARE_ENDPOINT(c.local, "192.168.1.10", 40000);
        QCOMPARE_ENDPOINT(c.remote, "192.168.1.11", 8080);
        QCOMPARE_COUNTERS(c, kOrigBytes, kReplBytes, kOrigPkts, kReplPkts);
    }
};

QTEST_APPLESS_MAIN(TestConntrackOrient)
#include "test_conntrack_orient.moc"
