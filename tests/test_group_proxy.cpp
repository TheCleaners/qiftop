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
        if (role == ConnectionModel::SortRole) {
            switch (static_cast<ConnectionModel::Column>(i.column())) {
            case ConnectionModel::Column::RxRate:  return double(c.rxBytes);
            case ConnectionModel::Column::TxRate:  return double(c.txBytes);
            case ConnectionModel::Column::RxTotal: return static_cast<qulonglong>(c.rxBytes);
            case ConnectionModel::Column::TxTotal: return static_cast<qulonglong>(c.txBytes);
            case ConnectionModel::Column::Iface:   return c.iface;
            default: return {};
            }
        }
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
    void addRow(Connection c) {
        const int row = rows.size();
        beginInsertRows({}, row, row);
        rows.append(std::move(c));
        endInsertRows();
    }
    void setRow(int row, Connection c, const QVector<int> &roles) {
        rows[row] = std::move(c);
        emit dataChanged(index(row, 0), index(row, columnCount() - 1), roles);
    }
    void emitRowChanged(int row, const QVector<int> &roles) {
        emit dataChanged(index(row, 0), index(row, columnCount() - 1), roles);
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

    void flatPassthroughOnInsert()
    {
        StubFlows src;
        src.replace({mk("eth0", "docker", "abc", 100, "nginx")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);

        QSignalSpy reset(&p, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&p, &QAbstractItemModel::rowsInserted);
        src.addRow(mk("wlan0", "", "", 0, ""));

        QCOMPARE(reset.size(), 0);
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(p.rowCount(), 2);
        QCOMPARE(p.mapToSource(p.index(1, 0)).row(), 1);
    }

    void flatPassthroughOnDataChanged()
    {
        StubFlows src;
        src.replace({mk("eth0", "", "", 0, "")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);

        QSignalSpy reset(&p, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&p, &QAbstractItemModel::dataChanged);
        src.emitRowChanged(0, {ConnectionModel::RxRateRole});

        QCOMPARE(reset.size(), 0);
        QCOMPARE(changed.size(), 1);
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

    void groupedDoesNotResetOnValueDataChanged()
    {
        StubFlows src;
        src.replace({mk("eth0",  "", "", 0, "", 100, 10),
                     mk("wlan0", "", "", 0, "", 200, 20)});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);

        QSignalSpy reset(&p, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&p, &QAbstractItemModel::dataChanged);
        src.emitRowChanged(0, {ConnectionModel::RxRateRole});

        QCOMPARE(reset.size(), 0);
        QVERIFY(changed.size() > 0);
        QCOMPARE(p.rowCount(), 2);
    }

    void groupedRebuildsOnKeyChange()
    {
        StubFlows src;
        src.replace({mk("eth0",  "", "", 0, ""),
                     mk("wlan0", "", "", 0, "")});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);
        QCOMPARE(p.rowCount(), 2);

        QSignalSpy reset(&p, &QAbstractItemModel::modelReset);
        Connection moved = src.rows[0];
        moved.iface = QStringLiteral("wlan0");
        src.setRow(0, moved, {ConnectionModel::ConnectionRole});

        QCOMPARE(reset.size(), 1);
        QCOMPARE(p.rowCount(), 1);
        const QModelIndex group = p.index(0, 0);
        QVERIFY(p.isGroupIndex(group));
        QCOMPARE(p.rowCount(group), 2);
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

    // Pins UIUX-C2: header click on the connections view must actually
    // reorder rows. Pre-fix, ConnectionGroupProxy had no sort() override
    // so QAbstractItemModel's no-op was inherited and every
    // sortByColumn / header-click dispatch was silently dropped.
    void groupedModeSortsGroupsByAggregatedValue()
    {
        StubFlows src;
        // Three interfaces, each with one flow of different rx — groups
        // are eth0=10, wlan0=300, lo=70. Descending by RxRate should
        // put wlan0 first.
        src.replace({mk("eth0",  "", "", 0, "",  10, 0),
                     mk("wlan0", "", "", 0, "", 300, 0),
                     mk("lo",    "", "", 0, "",  70, 0)});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);

        const int rxCol = static_cast<int>(ConnectionModel::Column::RxRate);
        p.sort(rxCol, Qt::DescendingOrder);

        QCOMPARE(p.rowCount(), 3);
        QCOMPARE(p.index(0, rxCol).data(ConnectionModel::RxRateRole).toDouble(), 300.0);
        QCOMPARE(p.index(1, rxCol).data(ConnectionModel::RxRateRole).toDouble(),  70.0);
        QCOMPARE(p.index(2, rxCol).data(ConnectionModel::RxRateRole).toDouble(),  10.0);

        p.sort(rxCol, Qt::AscendingOrder);
        QCOMPARE(p.index(0, rxCol).data(ConnectionModel::RxRateRole).toDouble(),  10.0);
        QCOMPARE(p.index(2, rxCol).data(ConnectionModel::RxRateRole).toDouble(), 300.0);
    }

    void groupedModeSortsChildrenWithinEachGroup()
    {
        StubFlows src;
        // Two interfaces; eth0 has three flows of varying rx.
        src.replace({mk("eth0",  "", "", 0, "",  50, 0),
                     mk("eth0",  "", "", 0, "", 500, 0),
                     mk("eth0",  "", "", 0, "", 200, 0),
                     mk("wlan0", "", "", 0, "",  10, 0)});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);

        const int rxCol = static_cast<int>(ConnectionModel::Column::RxRate);
        p.sort(rxCol, Qt::DescendingOrder);

        // First group is eth0 (sum=750 > wlan0's 10). Its children should
        // be ordered 500, 200, 50.
        const QModelIndex eth = p.index(0, 0);
        QVERIFY(p.isGroupIndex(eth));
        QCOMPARE(p.rowCount(eth), 3);
        QCOMPARE(p.index(0, rxCol, eth).data(ConnectionModel::RxRateRole).toDouble(), 500.0);
        QCOMPARE(p.index(1, rxCol, eth).data(ConnectionModel::RxRateRole).toDouble(), 200.0);
        QCOMPARE(p.index(2, rxCol, eth).data(ConnectionModel::RxRateRole).toDouble(),  50.0);
    }

    void groupedModeSortSurvivesRebuild()
    {
        // After a sort, a dataChanged that triggers an internal rebuild
        // (group-key change) must reapply the sort, not snap back to
        // source insertion order. Pins applyCurrentSort()'s rebuild-time
        // re-entry.
        StubFlows src;
        src.replace({mk("eth0",  "", "", 0, "", 100, 0),
                     mk("wlan0", "", "", 0, "", 200, 0)});
        ConnectionGroupProxy p;
        p.setSourceModel(&src);
        p.setViewMode(Settings::ConnectionViewMode::ByInterface);

        const int rxCol = static_cast<int>(ConnectionModel::Column::RxRate);
        p.sort(rxCol, Qt::DescendingOrder);
        QCOMPARE(p.index(0, rxCol).data(ConnectionModel::RxRateRole).toDouble(), 200.0);

        // Mutate row 0 to change its group key (eth0 → wifi0), which
        // forces ConnectionGroupProxy::onSourceDataChanged to fall into
        // the wholesale onSourceReset() rebuild path.
        src.setRow(0, mk("wifi0", "", "", 0, "", 100, 0),
                   {ConnectionModel::ConnectionRole});

        QCOMPARE(p.rowCount(), 2);  // still 2 groups, just renamed
        // Sort must still be applied: wlan0 (200) before wifi0 (100).
        QCOMPARE(p.index(0, rxCol).data(ConnectionModel::RxRateRole).toDouble(), 200.0);
        QCOMPARE(p.index(1, rxCol).data(ConnectionModel::RxRateRole).toDouble(), 100.0);
    }
};

QTEST_MAIN(TestConnectionGroupProxy)
#include "test_group_proxy.moc"
