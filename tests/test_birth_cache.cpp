// Unit tests for BirthCache — the bounded, TTL'd, direction-agnostic birth
// cache behind the eBPF birth+conntrack attribution hybrid. Pure data
// structure: no kernel, no BPF, no event loop. Births are injected directly.

#include <QTest>

#include "backend/BirthCache.h"

using namespace qiftop::backend;

namespace {

Connection mkFlow(L4Proto p, const char *l, quint16 lp,
                  const char *r, quint16 rp)
{
    Connection c;
    c.proto          = p;
    c.local.address  = QHostAddress(QString::fromLatin1(l));
    c.local.port     = lp;
    c.remote.address = QHostAddress(QString::fromLatin1(r));
    c.remote.port    = rp;
    return c;
}

BirthRecord mkRec(qint32 pid, const char *comm, quint64 startTime,
                  Direction dir = Direction::Outbound)
{
    BirthRecord r;
    r.pid             = pid;
    r.uid             = 1000;
    r.comm            = QString::fromLatin1(comm);
    r.direction       = dir;
    r.startTime       = startTime;
    r.firstSeenMonoMs = 1000;
    return r;
}

} // namespace

class TestBirthCache : public QObject {
    Q_OBJECT

private slots:
    void insertAndFindByFlowKey()
    {
        BirthCache cache;
        const auto flow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        cache.insert(birthKeyOf(flow), mkRec(4242, "curl", 99999), /*now=*/1000);

        const auto got = cache.find(flow, /*now=*/1500);
        QVERIFY(got.has_value());
        QCOMPARE(got->pid, qint32(4242));
        QCOMPARE(got->comm, QStringLiteral("curl"));
        QCOMPARE(got->startTime, quint64(99999));
        QVERIFY(got->valid());
    }

    void directionAgnosticKeyMatchesRegardlessOfFlowDirection()
    {
        // The conntrack flow's inferred direction differs from the birth's, and
        // its ifIndex too — the key must still match on the 5-tuple alone.
        BirthCache cache;
        Connection birthFlow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        birthFlow.direction = Direction::Outbound;
        birthFlow.ifIndex   = 7;
        cache.insert(birthKeyOf(birthFlow), mkRec(4242, "curl", 99999), 1000);

        Connection ctFlow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        ctFlow.direction = Direction::Unknown;   // conntrack couldn't tell
        ctFlow.ifIndex   = 3;                     // different egress device
        const auto got = cache.find(ctFlow, 1500);
        QVERIFY2(got.has_value(), "5-tuple match must ignore direction + ifIndex");
        QCOMPARE(got->pid, qint32(4242));
    }

    void missOnDifferentTuple()
    {
        BirthCache cache;
        cache.insert(birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443)),
                     mkRec(1, "a", 1), 1000);
        // Different remote port → different flow.
        const auto got = cache.find(mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 444), 1500);
        QVERIFY(!got.has_value());
        // UDP vs TCP on the same tuple is also a miss.
        const auto udp = cache.find(mkFlow(L4Proto::Udp, "10.0.0.1", 5000, "1.1.1.1", 443), 1500);
        QVERIFY(!udp.has_value());
    }

    void ttlExpiry()
    {
        BirthCache cache(/*maxEntries=*/100, /*ttlMs=*/5000);
        const auto flow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        cache.insert(birthKeyOf(flow), mkRec(7, "x", 1), /*now=*/1000);

        QVERIFY(cache.find(flow, /*now=*/5999).has_value());   // within TTL
        QVERIFY(!cache.find(flow, /*now=*/6001).has_value());  // expired (>5s)
    }

    void pruneReapsExpired()
    {
        BirthCache cache(100, 5000);
        cache.insert(birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 1, "1.1.1.1", 2)),
                     mkRec(1, "a", 1), 1000);
        cache.insert(birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 3, "1.1.1.1", 4)),
                     mkRec(2, "b", 2), 4000);
        QCOMPARE(cache.size(), 2);

        // At now=6500: first (inserted 1000) is >5s old, second (4000) is not.
        QCOMPARE(cache.prune(6500), 1);
        QCOMPARE(cache.size(), 1);
        QVERIFY(cache.find(mkFlow(L4Proto::Tcp, "10.0.0.1", 3, "1.1.1.1", 4), 6500).has_value());
    }

    void overflowClears()
    {
        BirthCache cache(/*maxEntries=*/2, /*ttlMs=*/100000);
        QVERIFY(!cache.insert(birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 1, "2.2.2.2", 9)), mkRec(1,"a",1), 1000));
        QVERIFY(!cache.insert(birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 2, "2.2.2.2", 9)), mkRec(2,"b",2), 1000));
        QCOMPARE(cache.size(), 2);
        // Third distinct key overflows the cap → full clear, then this one lands.
        const bool cleared = cache.insert(
            birthKeyOf(mkFlow(L4Proto::Tcp, "10.0.0.1", 3, "2.2.2.2", 9)), mkRec(3,"c",3), 1000);
        QVERIFY(cleared);
        QCOMPARE(cache.size(), 1);
        QVERIFY(cache.find(mkFlow(L4Proto::Tcp, "10.0.0.1", 3, "2.2.2.2", 9), 1000).has_value());
    }

    void replaceExistingKeyDoesNotOverflow()
    {
        BirthCache cache(/*maxEntries=*/1, 100000);
        const auto flow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        QVERIFY(!cache.insert(birthKeyOf(flow), mkRec(1, "old", 1), 1000));
        // Re-inserting the SAME key updates in place — must not trigger an
        // overflow-clear even at cap.
        QVERIFY(!cache.insert(birthKeyOf(flow), mkRec(2, "new", 2), 1100));
        QCOMPARE(cache.size(), 1);
        QCOMPARE(cache.find(flow, 1200)->pid, qint32(2));
    }

    void removeDropsEntry()
    {
        BirthCache cache;
        const auto flow = mkFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443);
        cache.insert(birthKeyOf(flow), mkRec(9, "z", 1), 1000);
        cache.remove(birthKeyOf(flow));
        QVERIFY(!cache.find(flow, 1000).has_value());
        QVERIFY(cache.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestBirthCache)
#include "test_birth_cache.moc"
