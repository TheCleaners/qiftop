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
        QCOMPARE(columnsFor(View::Interfaces).size(), 6);
        QCOMPARE(columnsFor(View::Connections).size(), 5);
        // Numeric columns are right-aligned; the first (name/flow) is not.
        QVERIFY(!columnsFor(View::Connections)[0].rightAlign);
        QVERIFY(columnsFor(View::Connections)[1].rightAlign);
    }

    void interfaceCells()
    {
        const QStringList cells = cellsForInterface(ifaceRow(QStringLiteral("eth0"), 0, 0, true));
        QCOMPARE(cells.size(), 6);
        QCOMPARE(cells[0], QStringLiteral("eth0"));
        QCOMPARE(cells[5], QStringLiteral("up"));
        // down + loopback render their own status strings.
        QCOMPARE(cellsForInterface(ifaceRow(QStringLiteral("x"), 0, 0, false))[5],
                 QStringLiteral("down"));
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
        // Column 1 = RX rate, descending => b(90), c(50), a(10).
        const QList<int> desc = sortedInterfaceIndices(rows, 1, true);
        QCOMPARE(desc, (QList<int>{1, 2, 0}));
        // Ascending reverses.
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
