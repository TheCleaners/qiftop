// Bounded top-K-by-bytes flow collector (FlowTopK.h) used by the in-process
// ConntrackMonitor to cap each snapshot at the loudest N flows, mirroring the
// agent's ConnectionsService cap. Pure logic — no live conntrack handle.

#include <QtTest/QtTest>

#include "backend/Connection.h"
#include "backend/linux/FlowTopK.h"

using qiftop::backend::linux::admitFlowTopK;
using qiftop::backend::linux::flowBytesTotal;

namespace {

Connection mk(quint64 rx, quint64 tx, quint16 lport = 0)
{
    Connection c;
    c.proto = L4Proto::Tcp;
    c.local.address  = QHostAddress(QStringLiteral("10.0.0.1"));
    c.local.port     = lport;
    c.remote.address = QHostAddress(QStringLiteral("1.1.1.1"));
    c.remote.port    = 443;
    c.rxBytes = rx;
    c.txBytes = tx;
    return c;
}

// Sorted descending total-bytes view of a heap, for order-independent compares.
QList<quint64> bytesDesc(const QList<Connection> &heap)
{
    QList<quint64> v;
    for (const auto &c : heap) v << flowBytesTotal(c);
    std::sort(v.begin(), v.end(), std::greater<>());
    return v;
}

} // namespace

class TestFlowTopK : public QObject {
    Q_OBJECT
private slots:
    // Below the cap: every flow kept (heap == set of all offered).
    void keepsAllBelowCap()
    {
        QList<Connection> heap;
        admitFlowTopK(heap, mk(10, 0), 4);
        admitFlowTopK(heap, mk(30, 0), 4);
        admitFlowTopK(heap, mk(20, 0), 4);
        QCOMPARE(heap.size(), 3);
        QCOMPARE(bytesDesc(heap), (QList<quint64>{30, 20, 10}));
    }

    // At the cap: keeps exactly the loudest `cap` flows regardless of arrival
    // order, dropping the smaller ones.
    void keepsLoudestAtCap()
    {
        QList<Connection> heap;
        const QList<quint64> offered = {5, 100, 1, 50, 7, 80, 3, 60};
        for (quint64 b : offered)
            admitFlowTopK(heap, mk(b, 0), 3);
        QCOMPARE(heap.size(), 3);
        // Top 3 by bytes are 100, 80, 60.
        QCOMPARE(bytesDesc(heap), (QList<quint64>{100, 80, 60}));
    }

    // rx and tx both count toward the ranking (total bytes).
    void ranksByTotalBytes()
    {
        QList<Connection> heap;
        admitFlowTopK(heap, mk(0, 0, 1), 2);     // total 0
        admitFlowTopK(heap, mk(10, 10, 2), 2);   // total 20
        admitFlowTopK(heap, mk(5, 30, 3), 2);    // total 35
        QCOMPARE(heap.size(), 2);
        QCOMPARE(bytesDesc(heap), (QList<quint64>{35, 20}));
    }

    // A flow equal to the current minimum does NOT evict (strictly-greater
    // admission), so ties at the boundary keep the incumbent — deterministic.
    void equalToMinDoesNotEvict()
    {
        QList<Connection> heap;
        admitFlowTopK(heap, mk(10, 0, 1), 2);
        admitFlowTopK(heap, mk(20, 0, 2), 2);
        // Heap is full {10,20}; offering another 10 must not displace anything.
        admitFlowTopK(heap, mk(10, 0, 99), 2);
        QCOMPARE(heap.size(), 2);
        QCOMPARE(bytesDesc(heap), (QList<quint64>{20, 10}));
        // The incumbent 10 (port 1), not the newcomer (port 99), is retained.
        bool sawPort1 = false, sawPort99 = false;
        for (const auto &c : heap) {
            if (flowBytesTotal(c) == 10 && c.local.port == 1)  sawPort1 = true;
            if (flowBytesTotal(c) == 10 && c.local.port == 99) sawPort99 = true;
        }
        QVERIFY(sawPort1);
        QVERIFY(!sawPort99);
    }

    // cap <= 0 means unbounded: every flow is appended, no heap maintenance.
    void capZeroIsUnbounded()
    {
        QList<Connection> heap;
        for (int i = 0; i < 1000; ++i)
            admitFlowTopK(heap, mk(quint64(i), 0), 0);
        QCOMPARE(heap.size(), 1000);
    }

    // The heap invariant holds after every operation: front() is the minimum,
    // so eviction always targets the true smallest.
    void frontIsAlwaysMin()
    {
        QList<Connection> heap;
        const QList<quint64> offered = {9, 2, 7, 4, 11, 1, 8, 6, 13, 3};
        for (quint64 b : offered) {
            admitFlowTopK(heap, mk(b, 0), 4);
            quint64 minSeen = std::numeric_limits<quint64>::max();
            for (const auto &c : heap) minSeen = std::min(minSeen, flowBytesTotal(c));
            QCOMPARE(flowBytesTotal(heap.constFirst()), minSeen);
        }
        QCOMPARE(bytesDesc(heap), (QList<quint64>{13, 11, 9, 8}));
    }
};

QTEST_APPLESS_MAIN(TestFlowTopK)
#include "test_flow_topk.moc"
