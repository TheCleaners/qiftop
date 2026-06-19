// Unit tests for DeepAttributionQueue — the bounded, deduplicating,
// priority-by-bytes queue behind the async deep-attribution pass (v0.4 §5).
// Pure: no Qt Widgets, no DBus, no kernel.

#include <QTest>

#include "backend/DeepAttributionQueue.h"

using namespace qiftop::backend;

namespace {
// Build a request whose key is distinguished by remotePort and whose priority
// (total bytes) is rx+tx.
DeepAttributionRequest mkReq(quint16 port, quint64 bytes, quint64 gen = 1,
                             int attempts = 0)
{
    Connection c;
    c.proto         = L4Proto::Tcp;
    c.local.address = QHostAddress(QStringLiteral("10.0.0.1"));
    c.local.port    = 1234;
    c.remote.address = QHostAddress(QStringLiteral("10.0.0.2"));
    c.remote.port   = port;
    c.rxBytes       = bytes;
    c.txBytes       = 0;

    DeepAttributionRequest r;
    r.key        = keyOf(c);
    r.flow       = c;
    r.generation = gen;
    r.attempts   = attempts;
    return r;
}
} // namespace

class TestDeepQueue : public QObject {
    Q_OBJECT

private slots:
    void dedupByKeyLatestGenerationWins()
    {
        DeepAttributionQueue q;
        q.enqueue({mkReq(80, 100, /*gen=*/1)});
        q.enqueue({mkReq(80, 999, /*gen=*/2)}); // same key, newer gen + bytes
        QCOMPARE(q.size(), 1);

        const auto out = q.dequeue(10);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.front().generation, quint64(2));
        QCOMPARE(out.front().flow.rxBytes, quint64(999));
    }

    void attemptsCarriedForwardOnMerge()
    {
        DeepAttributionQueue q;
        q.enqueue({mkReq(80, 100, 1, /*attempts=*/5)});
        // Fresh snapshot re-observes the same flow with attempts reset to 0;
        // the queue must keep the higher attempts so aging still works.
        q.enqueue({mkReq(80, 100, 2, /*attempts=*/0)});
        const auto out = q.dequeue(10);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.front().attempts, 5);
        QCOMPARE(out.front().generation, quint64(2));
    }

    void dequeueReturnsTopTalkersFirst()
    {
        DeepAttributionQueue q;
        q.enqueue({mkReq(80, 100), mkReq(81, 300), mkReq(82, 200)});
        QCOMPARE(q.size(), 3);

        const auto top2 = q.dequeue(2);
        QCOMPARE(top2.size(), 2);
        QCOMPARE(top2[0].flow.rxBytes, quint64(300)); // loudest first
        QCOMPARE(top2[1].flow.rxBytes, quint64(200));
        QCOMPARE(q.size(), 1);                          // quietest remains
        QCOMPARE(q.dequeue(10).front().flow.rxBytes, quint64(100));
    }

    void capacityDropsQuietest()
    {
        DeepAttributionQueue q(2);
        const int dropped =
            q.enqueue({mkReq(80, 100), mkReq(81, 300), mkReq(82, 200)});
        QCOMPARE(dropped, 1);
        QCOMPARE(q.size(), 2);
        const auto out = q.dequeue(10);
        // The quietest (100) was dropped; the two loudest survive.
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].flow.rxBytes, quint64(300));
        QCOMPARE(out[1].flow.rxBytes, quint64(200));
    }

    void clearEmptiesQueue()
    {
        DeepAttributionQueue q;
        q.enqueue({mkReq(80, 100), mkReq(81, 200)});
        QVERIFY(!q.isEmpty());
        q.clear();
        QVERIFY(q.isEmpty());
        QCOMPARE(q.dequeue(10).size(), 0);
    }

    void zeroCapacityKeepsNothing()
    {
        DeepAttributionQueue q(2);
        q.enqueue({mkReq(80, 100), mkReq(81, 200)});
        QCOMPARE(q.size(), 2);
        q.setCapacity(0);     // hard off — drops everything
        QCOMPARE(q.size(), 0);
        QCOMPARE(q.enqueue({mkReq(82, 300)}), 1); // and rejects new work
        QVERIFY(q.isEmpty());
    }
};

QTEST_MAIN(TestDeepQueue)
#include "test_deep_queue.moc"
