// Unit tests for NetworkModel's bandwidth-gauge surface — the role gating and
// the loopback-excluded view scale that the RowGaugeDelegate paints from.
// Widget-linked (the model pulls QApplication for the gauge tint) but headless.

#include <QSignalSpy>
#include <QTest>

#include "ui/GaugeRoles.h"
#include "ui/NetworkModel.h"

namespace {
InterfaceStats mkIface(const QString &name, quint64 rx, quint64 tx,
                       bool loopback = false)
{
    InterfaceStats s;
    s.name       = name;
    s.rxBytes    = rx;
    s.txBytes    = tx;
    s.isUp       = true;
    s.isLoopback = loopback;
    return s;
}

// Row index of `name` in the (name-sorted) model.
int rowOf(const NetworkModel &m, const QString &name)
{
    for (int r = 0; r < m.rowCount(); ++r)
        if (m.index(r, 0).data(Qt::DisplayRole).toString() == name)
            return r;
    return -1;
}

double frac(const NetworkModel &m, int row)
{
    const int col = static_cast<int>(NetworkModel::Column::RxRate);
    return m.index(row, col).data(qiftop::ui::GaugeFractionRole).toDouble();
}
} // namespace

class TestNetworkModel : public QObject {
    Q_OBJECT

private slots:
    void gaugeOffYieldsInvalidRoles()
    {
        NetworkModel m;
        m.updateStats({mkIface("eth0", 0, 0)});
        m.updateStats({mkIface("eth0", 1000, 1000)});

        const QModelIndex idx =
            m.index(0, static_cast<int>(NetworkModel::Column::RxRate));
        // Default: gauge disabled → both gauge roles invalid (delegate paints
        // nothing).
        QVERIFY(!idx.data(qiftop::ui::GaugeFractionRole).isValid());
        QVERIFY(!idx.data(qiftop::ui::GaugeDarkColorRole).isValid());
    }

    void gaugeOnYieldsBoundedFractionAndColor()
    {
        NetworkModel m;
        m.setThroughputGaugeEnabled(true);
        m.updateStats({mkIface("eth0", 0, 0)});
        m.updateStats({mkIface("eth0", 4096, 4096)});

        const QModelIndex idx =
            m.index(0, static_cast<int>(NetworkModel::Column::RxRate));
        const QVariant fv = idx.data(qiftop::ui::GaugeFractionRole);
        QVERIFY(fv.isValid());
        const double f = fv.toDouble();
        QVERIFY(f >= 0.0 && f <= 1.0);
        QVERIFY(idx.data(qiftop::ui::GaugeDarkColorRole).isValid());
    }

    // The headline guard: a very loud loopback must NOT shrink a physical
    // interface's gauge, because the view scale excludes loopback. The
    // loudest non-loopback link always lands at fraction >= ~0.4 (niceScale
    // rounds the leading digit up to 1/2/5/10), independent of the sample
    // interval — so > 0.3 cleanly distinguishes "loopback excluded" (this
    // impl) from "loopback included" (would push eth0 toward 0).
    void loopbackExcludedFromScale()
    {
        NetworkModel m;
        m.setThroughputGaugeEnabled(true);
        m.updateStats({mkIface("eth0", 0, 0),
                       mkIface("lo", 0, 0, /*loopback=*/true)});
        m.updateStats({mkIface("eth0", 2000, 2000),
                       mkIface("lo", 50'000'000, 50'000'000, true)});

        const int eth = rowOf(m, QStringLiteral("eth0"));
        QVERIFY(eth >= 0);
        QVERIFY2(frac(m, eth) > 0.3,
                 "loud loopback must not flatten the physical interface gauge");

        // Loopback still paints its own bar (clamped to full since its rate
        // exceeds the physical-link scale).
        const int lo = rowOf(m, QStringLiteral("lo"));
        QVERIFY(lo >= 0);
        QCOMPARE(frac(m, lo), 1.0);
    }

    void toggleEmitsDataChanged()
    {
        NetworkModel m;
        m.updateStats({mkIface("eth0", 0, 0)});
        QSignalSpy spy(&m, &QAbstractItemModel::dataChanged);
        m.setThroughputGaugeEnabled(true);
        QCOMPARE(spy.count(), 1);
        // Idempotent: no redundant repaint when the value doesn't change.
        m.setThroughputGaugeEnabled(true);
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(TestNetworkModel)
#include "test_network_model.moc"
