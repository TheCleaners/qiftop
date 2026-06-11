// Unit tests for InterfaceAggregator — the plain-QObject rate/row aggregator
// extracted from NetworkModel (libqiftop). No QAbstractItemModel, no Widgets.

#include <QSignalSpy>
#include <QTest>

#include "aggregate/InterfaceAggregator.h"

using qiftop::aggregate::InterfaceAggregator;

namespace {
InterfaceStats mkIface(const QString &name, quint64 rx, quint64 tx, bool up = true)
{
    InterfaceStats s;
    s.name    = name;
    s.rxBytes = rx;
    s.txBytes = tx;
    s.isUp    = up;
    return s;
}
} // namespace

class TestInterfaceAggregator : public QObject {
    Q_OBJECT

private slots:
    void firstSnapshotHasZeroRatesAndResets()
    {
        InterfaceAggregator agg;
        QSignalSpy resetSpy(&agg, &InterfaceAggregator::didReset);

        agg.updateStats({mkIface("eth0", 1000, 500), mkIface("lo", 0, 0)});

        QCOMPARE(agg.rowCount(), 2);
        QCOMPARE(resetSpy.count(), 1);
        // No previous sample => rates are 0 on the first tick.
        for (const auto &r : agg.rows()) {
            QCOMPARE(r.rxRate, 0.0);
            QCOMPARE(r.txRate, 0.0);
        }
        // Sorted by name: eth0 before lo.
        QCOMPARE(agg.rowAt(0).current.name, QStringLiteral("eth0"));
        QCOMPARE(agg.rowAt(1).current.name, QStringLiteral("lo"));
    }

    void secondSnapshotComputesPositiveRatesInPlace()
    {
        InterfaceAggregator agg;
        agg.updateStats({mkIface("eth0", 1000, 500)});

        QSignalSpy rowsSpy(&agg, &InterfaceAggregator::rowsChanged);
        QSignalSpy resetSpy(&agg, &InterfaceAggregator::didReset);

        // Wait a beat so the elapsed-time delta is non-trivial, then deliver a
        // snapshot with more bytes. Same row set => in-place update, no reset.
        QTest::qWait(50);
        agg.updateStats({mkIface("eth0", 5000, 2500)});

        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(rowsSpy.count(), 1);
        QCOMPARE(agg.rowCount(), 1);
        QVERIFY(agg.rowAt(0).rxRate > 0.0);
        QVERIFY(agg.rowAt(0).txRate > 0.0);
        // rx delta (4000) is double tx delta (2000) over the same interval.
        QVERIFY(qFuzzyCompare(agg.rowAt(0).rxRate, 2.0 * agg.rowAt(0).txRate));
    }

    void addingAnInterfaceResets()
    {
        InterfaceAggregator agg;
        agg.updateStats({mkIface("eth0", 0, 0)});

        QSignalSpy resetSpy(&agg, &InterfaceAggregator::didReset);
        QSignalSpy rowsSpy(&agg, &InterfaceAggregator::rowsChanged);

        agg.updateStats({mkIface("eth0", 0, 0), mkIface("wlan0", 0, 0)});

        QCOMPARE(agg.rowCount(), 2);
        QCOMPARE(resetSpy.count(), 1);   // structure changed
        QCOMPARE(rowsSpy.count(), 0);
    }
};

QTEST_MAIN(TestInterfaceAggregator)
#include "test_interface_aggregator.moc"
