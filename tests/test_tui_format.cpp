// Unit tests for the pure-logic TUI formatting + sorting (src/tui/TuiFormat.h).
// No ncurses, no event loop.

#include <QHostAddress>
#include <QTest>

#include "tui/TuiFormat.h"

using namespace qiftop::tui;
using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;

namespace {

InterfaceAggregator::Row ifaceRow(const QString &name, double rxRate,
                                  quint64 rxBytes, bool up = true)
{
    InterfaceAggregator::Row r;
    r.current.name    = name;
    r.current.isUp    = up;
    r.current.rxBytes = rxBytes;
    r.rxRate          = rxRate;
    return r;
}

ConnectionAggregator::Row connRow(const char *remote, double rxRate)
{
    ConnectionAggregator::Row r;
    r.current.remote.address = QHostAddress(QString::fromLatin1(remote));
    r.rxRate                 = rxRate;
    return r;
}

} // namespace

class TestTuiFormat : public QObject {
    Q_OBJECT

private slots:
    void columnsMatchView()
    {
        // [name/flow] [RX rate] [TX rate] [RX total] [TX total] (+Status).
        // The bandwidth gauge is a row-spanning background fill, not a column.
        QCOMPARE(columnsFor(View::Interfaces).size(), 6);
        QCOMPARE(columnsFor(View::Connections).size(), 5);
        // Column 0 (name/flow) is left-aligned; rate columns (1..) right.
        QVERIFY(!columnsFor(View::Connections)[0].rightAlign);
        QVERIFY(columnsFor(View::Connections)[1].rightAlign);
        // Numeric columns are fixed-width (kills per-frame jitter); name flexes.
        QCOMPARE(columnsFor(View::Connections)[0].fixedWidth, 0);
        QCOMPARE(columnsFor(View::Connections)[1].fixedWidth, kRateW);
    }

    void interfaceCells()
    {
        const QStringList cells = cellsForInterface(ifaceRow(QStringLiteral("eth0"), 0, 0, true));
        QCOMPARE(cells.size(), 6);
        QCOMPARE(cells[0], QStringLiteral("eth0"));
        QCOMPARE(cells[5], QStringLiteral("up"));
        QCOMPARE(cellsForInterface(ifaceRow(QStringLiteral("x"), 0, 0, false))[5],
                 QStringLiteral("down"));
    }

    void scaleAndGaugeFraction()
    {
        // niceScale rounds up to 1/2/5 × 10^k.
        QCOMPARE(niceScale(0.0), 1024.0);
        QVERIFY(niceScale(900.0) >= 900.0);
        QVERIFY(niceScale(1.5e6) >= 1.5e6);
        // gaugeFraction is the row's combined rate against the view scale,
        // clamped to [0,1]. Screen turns this into a background fill width.
        QCOMPARE(gaugeFraction(100, 100), 1.0);
        QCOMPARE(gaugeFraction(0, 100), 0.0);
        QCOMPARE(gaugeFraction(50, 100), 0.5);
        QCOMPARE(gaugeFraction(200, 100), 1.0);  // clamped
        QCOMPARE(gaugeFraction(5, 0), 0.0);       // guard against /0
    }

    void connectionCellsRenderFlow()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        Connection c;
        c.proto          = L4Proto::Tcp;
        c.local.address  = QHostAddress(QStringLiteral("10.0.0.1"));
        c.local.port     = 5000;
        c.remote.address = QHostAddress(QStringLiteral("1.1.1.1"));
        c.remote.port    = 443;
        c.direction      = Direction::Outbound;
        agg.updateConnections({c});
        QCOMPARE(agg.rowCount(), 1);

        const QStringList cells = cellsForConnection(agg, agg.rowAt(0));
        QCOMPARE(cells.size(), 5);
        QVERIFY(cells[0].contains(QStringLiteral("TCP")));
        QVERIFY(cells[0].contains(QStringLiteral("10.0.0.1:5000")));
        QVERIFY(cells[0].contains(QStringLiteral("1.1.1.1:443")));
        QVERIFY(cells[0].contains(QChar(0x2192))); // →
    }

    void interfaceSortByRateDesc()
    {
        const QList<InterfaceAggregator::Row> rows = {
            ifaceRow(QStringLiteral("a"), 10, 0),
            ifaceRow(QStringLiteral("b"), 90, 0),
            ifaceRow(QStringLiteral("c"), 50, 0),
        };
        // Column 1 = bandwidth (combined; tx==0 so == rx rate), descending.
        const QList<int> desc = sortedInterfaceIndices(rows, 1, true);
        QCOMPARE(desc, (QList<int>{1, 2, 0}));
        const QList<int> asc = sortedInterfaceIndices(rows, 1, false);
        QCOMPARE(asc, (QList<int>{0, 2, 1}));
    }

    void interfaceSortByNameAsc()
    {
        const QList<InterfaceAggregator::Row> rows = {
            ifaceRow(QStringLiteral("wlan0"), 0, 0),
            ifaceRow(QStringLiteral("eth0"), 0, 0),
            ifaceRow(QStringLiteral("lo"), 0, 0),
        };
        // Column 0 = name, ascending => eth0, lo, wlan0.
        QCOMPARE(sortedInterfaceIndices(rows, 0, false), (QList<int>{1, 2, 0}));
    }

    void connectionSortByRateDesc()
    {
        const QList<ConnectionAggregator::Row> rows = {
            connRow("1.1.1.1", 5),
            connRow("2.2.2.2", 100),
            connRow("3.3.3.3", 40),
        };
        QCOMPARE(sortedConnectionIndices(rows, 1, true), (QList<int>{1, 2, 0}));
    }

    void settingsModelMatchesEnum()
    {
        // One row per Setting (except the Count sentinel), in enum order.
        const QList<SettingRow> rows =
            settingsRows(QStringLiteral("dark"), true, false, true, true);
        QCOMPARE(rows.size(), static_cast<int>(Setting::Count));
        QCOMPARE(rows[static_cast<int>(Setting::Theme)].value, QStringLiteral("dark"));
        QCOMPARE(rows[static_cast<int>(Setting::Gauge)].value, QStringLiteral("on"));
        QCOMPARE(rows[static_cast<int>(Setting::Dns)].value, QStringLiteral("off"));
        QCOMPARE(rows[static_cast<int>(Setting::UdpAggregate)].value, QStringLiteral("on"));
        QCOMPARE(rows[static_cast<int>(Setting::Smoothing)].value, QStringLiteral("on"));
        // Every row carries a label + a non-empty help string.
        for (const SettingRow &r : rows) {
            QVERIFY(!r.label.isEmpty());
            QVERIFY(!r.help.isEmpty());
        }
    }

    void sortFieldsModel()
    {
        // Fields overlay lists every column; only the active sort column shows
        // a direction marker, and it flips with the descending flag.
        const QList<Column> cols = columnsFor(View::Connections);
        const QList<SettingRow> desc = sortFieldRows(cols, 1, true);
        QCOMPARE(desc.size(), cols.size());
        QVERIFY(desc[1].value.contains(QStringLiteral("desc")));
        QVERIFY(desc[0].value.isEmpty());        // non-sort columns: no marker
        QVERIFY(!desc[1].label.isEmpty());
        QVERIFY(!desc[1].help.isEmpty());
        const QList<SettingRow> asc = sortFieldRows(cols, 0, false);
        QVERIFY(asc[0].value.contains(QStringLiteral("asc")));
        QVERIFY(asc[1].value.isEmpty());
    }
};

QTEST_MAIN(TestTuiFormat)
#include "test_tui_format.moc"
