// Tests for ConnectionGroupProxy: pass-through behaviour in Flat mode
// (so the v0.1 view geometry stays pixel-identical) and basic
// tree-of-groups shape in the by-X modes. Drives the proxy directly
// against a tiny stub source model that returns plain Connection
// values via the ConnectionRole — no Qt Widgets, no real
// ConnectionModel needed.

#include <QAbstractTableModel>
#include <QSignalSpy>
#include <QTest>

#include "ui/ConnectionGroupProxy.h"
#include "ui/ConnectionModel.h"
#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

namespace {

class StubFlows : public QAbstractTableModel {
public:
    QList<Connection> rows;

    int rowCount(const QModelIndex &p = {}) const override
    { return p.isValid() ? 0 : static_cast<int>(rows.size()); }
    int columnCount(const QModelIndex & = {}) const override
    { return static_cast<int>(ConnectionModel::Column::ColumnCount); }
    QVariant data(const QModelIndex &i, int role) const override
    {
        if (!i.isValid()) return {};
        const Connection &c = rows[i.row()];
        if (role == ConnectionModel::ConnectionRole) return QVariant::fromValue(c);
        if (role == ConnectionModel::RxRateRole)     return double(c.rxBytes);  // pretend
        if (role == ConnectionModel::TxRateRole)     return double(c.txBytes);
        if (role == Qt::DisplayRole) {
            switch (static_cast<ConnectionModel::Column>(i.column())) {
            case ConnectionModel::Column::Iface: return c.iface;
            case ConnectionModel::Column::Flow:  return c.iface + QStringLiteral(":flow");
            default: return {};
            }
        }
        return {};
    }
    void replace(QList<Connection> data) {
        beginResetModel();
        rows = std::move(data);
        endResetModel();
    }
};

Connection mk(const char *iface, const char *runtime, const char *cid,
              qint32 pid, const char *comm,
              quint64 rx = 1000, quint64 tx = 500)
{
    Connection c;
    c.iface = QString::fromLatin1(iface);
    c.container.runtime = QString::fromLatin1(runtime);
    c.container.id      = QString::fromLatin1(cid);
    c.process.pid       = pid;
    c.process.comm      = QString::fromLatin1(comm);
    c.rxBytes = rx;
    c.txBytes = tx;
    return c;
}

} // namespace

class TestConnectionGroupProxy : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<Connection>("Connection");
    }

    void flatModeIsPassThrough()
    {
        StubFlows src;
        src.replace({mk("eth0", "docker", "abc", 100, "nginx"),
                     mk("eth0", "", "", 0, ""),
                     mk("wlan0", "docker", "xyz", 200, "redis")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        QCOMPARE(p.viewMode(), Settings::ConnectionViewMode::Flat);
        QCOMPARE(p.rowCount(), 3);
        QCOMPARE(p.columnCount(),
                 static_cast<int>(ConnectionModel::Column::ColumnCount));
        // Every top-level row maps 1:1 to a source row.
        for (int r = 0; r < 3; ++r) {
            const QModelIndex i = p.index(r, 0);
            QVERIFY(i.isValid());
            QVERIFY(!p.parent(i).isValid());
            QVERIFY(!p.isGroupIndex(i));
            QCOMPARE(p.rowCount(i), 0);  // strict 1-level
            QCOMPARE(p.mapToSource(i).row(), r);
        }
    }

    void byInterfaceBuildsExpectedGroups()
    {
        StubFlows src;
        src.replace({mk("eth0",  "", "", 0, ""),
                     mk("eth0",  "", "", 0, ""),
                     mk("wlan0", "", "", 0, ""),
                     mk("",      "", "", 0, "")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);
        // eth0, wlan0, (unattributed) — 3 groups.
        QCOMPARE(p.rowCount(), 3);
        // eth0 group has 2 children, others 1 each.
        int seenChildren = 0;
        for (int gi = 0; gi < 3; ++gi) {
            const QModelIndex g = p.index(gi, 0);
            QVERIFY(p.isGroupIndex(g));
            QVERIFY(!p.parent(g).isValid());
            seenChildren += p.rowCount(g);
        }
        QCOMPARE(seenChildren, 4);
    }

    void byContainerKeysIncludeRuntime()
    {
        StubFlows src;
        // Same id under two runtimes must NOT collapse into one group.
        src.replace({mk("eth0", "docker", "deadbeef", 1, "a"),
                     mk("eth0", "podman", "deadbeef", 2, "b"),
                     mk("eth0", "",       "",         0, "")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByContainer);
        QCOMPARE(p.rowCount(), 3);  // docker, podman, host
    }

    void aggregateRatesSum()
    {
        StubFlows src;
        src.replace({mk("eth0", "", "", 0, "", 100, 10),
                     mk("eth0", "", "", 0, "", 200, 20),
                     mk("eth0", "", "", 0, "", 700, 70)});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);
        QCOMPARE(p.rowCount(), 1);
        const QModelIndex g = p.index(
            0, static_cast<int>(ConnectionModel::Column::RxRate));
        const double rxSum = g.data(ConnectionModel::RxRateRole).toDouble();
        const double txSum = g.data(ConnectionModel::TxRateRole).toDouble();
        QCOMPARE(rxSum, 1000.0);
        QCOMPARE(txSum,  100.0);
        // Group's SortRole on RxRate column equals the rx sum.
        QCOMPARE(g.data(ConnectionModel::SortRole).toDouble(), 1000.0);
    }

    void switchingModesResetsModel()
    {
        StubFlows src;
        src.replace({mk("eth0", "", "", 0, ""),
                     mk("wlan0", "", "", 0, "")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        QSignalSpy reset(&p, &QAbstractItemModel::modelReset);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(p.rowCount(), 2);
        p.setViewMode(Settings::ConnectionViewMode::Flat);
        QCOMPARE(reset.size(), 2);
        QCOMPARE(p.rowCount(), 2);  // back to source row count
        QVERIFY(!p.isGroupIndex(p.index(0, 0)));
    }
};

QTEST_MAIN(TestConnectionGroupProxy)
#include "test_group_proxy.moc"
