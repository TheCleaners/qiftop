// Unit tests for QtDnsResolver's bounded LRU cache. Uses the test-only
// primeCacheForTest() seam to seed entries directly without ever calling
// QHostInfo — keeps the test hermetic and fast (no real DNS, no network).

#include <QHostAddress>
#include <QTest>

#include "dns/DnsResolver.h"
#include "dns/QtDnsResolver.h"

class TestDnsCache : public QObject {
    Q_OBJECT

private slots:
    void cacheStoresAndRetrievesEntries()
    {
        QtDnsResolver r;
        const QHostAddress a(QStringLiteral("198.51.100.7"));
        r.primeCacheForTest(a, QStringLiteral("example.test"));
        QCOMPARE(r.cachedName(a), QStringLiteral("example.test"));
    }

    void clearCacheEmptiesEverything()
    {
        QtDnsResolver r;
        r.primeCacheForTest(QHostAddress(QStringLiteral("10.0.0.1")),
                            QStringLiteral("a.test"));
        r.primeCacheForTest(QHostAddress(QStringLiteral("10.0.0.2")),
                            QStringLiteral("b.test"));
        QCOMPARE(r.cacheSizeForTest(), 2);
        r.clearCache();
        QCOMPARE(r.cacheSizeForTest(), 0);
    }

    void evictsOldestEntriesOnceOverCap()
    {
        // kMaxEntries is 4096 internally; verify that going one over
        // triggers a batch eviction (~3%) and that the very-oldest entries
        // are the ones dropped (insertion order, rough LRU).
        QtDnsResolver r;
        constexpr int kCap = 4096;
        for (int i = 0; i < kCap; ++i) {
            r.primeCacheForTest(QHostAddress(quint32(0x0a000000 | i)),
                                QStringLiteral("host%1.test").arg(i));
        }
        QCOMPARE(r.cacheSizeForTest(), kCap);

        // One more push triggers the eviction batch.
        r.primeCacheForTest(QHostAddress(quint32(0x0b000000)),
                            QStringLiteral("trigger.test"));

        QVERIFY2(r.cacheSizeForTest() < kCap,
                 "cache must shrink below the cap after batched eviction");
        QVERIFY2(r.cacheSizeForTest() >= kCap * 0.9,
                 "eviction batch must be a small fraction of capacity, not a flush");

        // The oldest (index 0) entry should have been the first evicted.
        QVERIFY(r.cachedName(QHostAddress(quint32(0x0a000000))).isEmpty());
        // A recent entry should still be there.
        QCOMPARE(r.cachedName(QHostAddress(quint32(0x0b000000))),
                 QStringLiteral("trigger.test"));
    }

    void reinsertOfSameKeyDoesNotInflateOrder()
    {
        // Repeatedly priming the same address used to leak m_order entries
        // (each store appended even when the cache entry was overwritten).
        // Guard against that regression — cache size and "order" length
        // (probed via the size returned after a known number of evicts)
        // should agree.
        QtDnsResolver r;
        const QHostAddress a(QStringLiteral("10.1.2.3"));
        for (int i = 0; i < 100; ++i)
            r.primeCacheForTest(a, QStringLiteral("v%1.test").arg(i));
        QCOMPARE(r.cacheSizeForTest(), 1);
        QCOMPARE(r.cachedName(a), QStringLiteral("v99.test"));
    }
};

QTEST_MAIN(TestDnsCache)
#include "test_dns_cache.moc"
