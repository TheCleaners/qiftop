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
        // [name/flow] [bar] [RX rate] [TX rate] [RX total] [TX total] (+Status)
        QCOMPARE(columnsFor(View::Interfaces).size(), 7);
        QCOMPARE(columnsFor(View::Connections).size(), 6);
        // Column 0 (name/flow) and the bar (1) are left-aligned; rates (2..) right.
        QVERIFY(!columnsFor(View::Connections)[0].rightAlign);
        QVERIFY(!columnsFor(View::Connections)[kBarColumn].rightAlign);
        QVERIFY(columnsFor(View::Connections)[2].rightAlign);
        // The bar column is fixed-width.
        QCOMPARE(columnsFor(View::Connections)[kBarColumn].fixedWidth, kBarWidth);
    }

    void interfaceCells()
    {
        const QStringList cells = cellsForInterface(ifaceRow(QStringLiteral("eth0"), 0, 0, true));
        QCOMPARE(cells.size(), 7);
        QCOMPARE(cells[0], QStringLiteral("eth0"));
        QVERIFY(cells[kBarColumn].isEmpty());  // bar filled in by the caller
        QCOMPARE(cells[6], QStringLiteral("up"));
        QCOMPARE(cellsForInterface(ifaceRow(QStringLiteral("x"), 0, 0, false))[6],
                 QStringLiteral("down"));
    }

    void barScalesAndFills()
    {
        // niceScale rounds up to 1/2/5 × 10^k.
        QCOMPARE(niceScale(0.0), 1024.0);
        QVERIFY(niceScale(900.0) >= 900.0);
        QVERIFY(niceScale(1.5e6) >= 1.5e6);
        // A full-rate bar fills the width; half fills ~half; zero is blank;
        // a tiny non-zero rate still shows at least one cell.
        const QChar block(0x2588);
        QCOMPARE(barString(100, 100, 10).count(block), 10);
        QCOMPARE(barString(0, 100, 10).count(block), 0);
        QCOMPARE(barString(50, 100, 10).count(block), 5);
        QVERIFY(barString(1, 1e9, 10).count(block) >= 1); // non-zero floor
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
        QCOMPARE(cells.size(), 6);
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
};

QTEST_MAIN(TestTuiFormat)
#include "test_tui_format.moc"
