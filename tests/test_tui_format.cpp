// Unit tests for the pure-logic TUI formatting + sorting (src/tui/TuiFormat.h).
// No ncurses, no event loop.

#include <QHostAddress>
#include <QTest>

#include "tui/Expansion.h"
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
        // Connections: [flow][RX rate][TX rate][RX total][TX total].
        // Interfaces add [MTU][State][Err/Drop] (the Qt-UI-style extra info).
        QCOMPARE(columnsFor(View::Interfaces).size(), 8);
        QCOMPARE(columnsFor(View::Connections).size(), 5);
        // Column 0 (name/flow) is left-aligned; rate columns (1..) right.
        QVERIFY(!columnsFor(View::Connections)[0].rightAlign);
        QVERIFY(columnsFor(View::Connections)[1].rightAlign);
        // Numeric columns are fixed-width (kills per-frame jitter); name flexes.
        QCOMPARE(columnsFor(View::Connections)[0].fixedWidth, 0);
        QCOMPARE(columnsFor(View::Connections)[1].fixedWidth, kRateW);
        QCOMPARE(columnsFor(View::Interfaces)[5].title, QStringLiteral("MTU"));
        QCOMPARE(columnsFor(View::Interfaces)[6].title, QStringLiteral("State"));
    }

    void interfaceCells()
    {
        const QStringList cells = cellsForInterface(ifaceRow(QStringLiteral("eth0"), 0, 0, true));
        QCOMPARE(cells.size(), 8);
        QVERIFY(cells[0].startsWith(QStringLiteral("eth0")));
        // State column (index 6) reflects up/down (operState 0 -> isUp fallback).
        QCOMPARE(cells[6], QStringLiteral("up"));
        QCOMPARE(cellsForInterface(ifaceRow(QStringLiteral("x"), 0, 0, false))[6],
                 QStringLiteral("down"));
        // Err/Drop column (index 7) is "<errs>/<drops>".
        QCOMPARE(cells[7], QStringLiteral("0/0"));
    }

    void operStateLabels()
    {
        QCOMPARE(operStateText(6, false), QStringLiteral("up"));
        QCOMPARE(operStateText(2, true),  QStringLiteral("down"));
        QCOMPARE(operStateText(5, false), QStringLiteral("dormant"));
        QCOMPARE(operStateText(0, true),  QStringLiteral("up"));   // fallback to isUp
        QCOMPARE(operStateText(0, false), QStringLiteral("down"));
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

    void groupingKeysAndLabels()
    {
        Connection c;
        c.iface = QStringLiteral("eth0");
        // Interface grouping keys off the iface name.
        QCOMPARE(groupKeyFor(GroupBy::Interface, c), QStringLiteral("eth0"));
        QCOMPARE(groupLabelFor(GroupBy::Interface, c), QStringLiteral("eth0"));
        Connection bare;  // no iface -> unattributed bucket (empty key)
        QVERIFY(groupKeyFor(GroupBy::Interface, bare).isEmpty());
        QCOMPARE(groupLabelFor(GroupBy::Interface, bare), QStringLiteral("(unattributed)"));

        // Container grouping includes the runtime so docker/podman never collide.
        Connection d; d.container = {QStringLiteral("docker"), QStringLiteral("abc123"), QStringLiteral("web")};
        Connection p; p.container = {QStringLiteral("podman"), QStringLiteral("abc123"), QStringLiteral("web")};
        QVERIFY(groupKeyFor(GroupBy::Container, d) != groupKeyFor(GroupBy::Container, p));
        QVERIFY(groupLabelFor(GroupBy::Container, d).contains(QStringLiteral("docker")));
        QCOMPARE(groupLabelFor(GroupBy::Container, bare), QStringLiteral("(no container)"));

        // Process grouping uses pid+comm; unattributed when pid==0.
        Connection pr; pr.process = {1234, QStringLiteral("curl"), {}, {}, 0};
        QVERIFY(groupLabelFor(GroupBy::Process, pr).contains(QStringLiteral("curl")));
        QVERIFY(groupLabelFor(GroupBy::Process, pr).contains(QStringLiteral("1234")));
        QCOMPARE(groupLabelFor(GroupBy::Process, bare), QStringLiteral("(unattributed)"));

        QCOMPARE(groupByName(GroupBy::None), QStringLiteral("off"));
        QCOMPARE(groupByName(GroupBy::Container), QStringLiteral("container"));
        // Round-trip via groupByFromName (CLI / config parsing, with aliases).
        QCOMPARE(groupByFromName(QStringLiteral("iface")), GroupBy::Interface);
        QCOMPARE(groupByFromName(QStringLiteral("PROCESS")), GroupBy::Process);
        QCOMPARE(groupByFromName(QStringLiteral("off")), GroupBy::None);
        QCOMPARE(groupByFromName(QStringLiteral("bogus")), GroupBy::Count); // unrecognised
    }

    // wrapToWidth: word-wrap with hard-break for over-long tokens (the helper
    // behind the modal dialogs' wrapped value column).
    void wrapToWidthWordWrap()
    {
        // Simple word wrap at a boundary.
        QCOMPARE(wrapToWidth(QStringLiteral("the quick brown fox"), 9),
                 (QStringList{QStringLiteral("the quick"), QStringLiteral("brown fox")}));
        // Fits on one line → single element.
        QCOMPARE(wrapToWidth(QStringLiteral("short"), 20),
                 (QStringList{QStringLiteral("short")}));
        // Empty → one empty line (so a spacer renders as a blank row).
        QCOMPARE(wrapToWidth(QString(), 10), (QStringList{QString()}));
        // Every line is within the width.
        for (const QString &l : wrapToWidth(
                 QStringLiteral("page up down half page navigation keys here"), 12))
            QVERIFY(l.size() <= 12);
    }

    void wrapToWidthHardBreaksLongToken()
    {
        // A token longer than the width (e.g. a long path / cmdline) is split
        // across lines rather than overflowing.
        const QString path = QStringLiteral("/usr/lib/x86_64-linux-gnu/very/long/path/binary");
        const QStringList lines = wrapToWidth(path, 10);
        for (const QString &l : lines)
            QVERIFY(l.size() <= 10);
        // Reassembling the (space-free) token reproduces it.
        QCOMPARE(lines.join(QString()), path);
        // Width <= 0 is clamped to 1, not a crash / infinite loop.
        QVERIFY(!wrapToWidth(QStringLiteral("ab"), 0).isEmpty());
    }

    // groupDetailRows: the group-info window content (Enter on a header).
    void groupDetailRowsContent()
    {
        Connection rep;
        rep.process = {4242, QStringLiteral("postgres"), {}, {}, 0};
        rep.process.uid = 70;
        const auto labels = [](const QList<SettingRow> &rs) {
            QStringList out; for (const auto &r : rs) out << r.label; return out;
        };
        // Process group, no on-demand details yet: bulk fields + aggregates.
        auto rows = groupDetailRows(GroupBy::Process, rep,
                                    1000.0, 500.0, 4096, 2048, 7, nullptr);
        const QStringList l = labels(rows);
        QVERIFY(l.contains(QStringLiteral("Process")));
        QVERIFY(l.contains(QStringLiteral("PID")));
        QVERIFY(l.contains(QStringLiteral("UID")));
        QVERIFY(l.contains(QStringLiteral("Flows")));
        QVERIFY(l.contains(QStringLiteral("RX rate")));
        QVERIFY(!l.contains(QStringLiteral("Exe")));   // not fetched yet

        // With on-demand details, exe/cmdline/cwd appear.
        qiftop::backend::ProcessDetails pd;
        pd.pid = 4242; pd.exe = QStringLiteral("/usr/lib/postgresql/bin/postgres");
        pd.cmdline = QStringLiteral("postgres -D /var/lib/pg");
        pd.cwd = QStringLiteral("/var/lib/pg");
        rows = groupDetailRows(GroupBy::Process, rep,
                               1000.0, 500.0, 4096, 2048, 7, &pd);
        const QStringList l2 = labels(rows);
        QVERIFY(l2.contains(QStringLiteral("Exe")));
        QVERIFY(l2.contains(QStringLiteral("Cmdline")));
        QVERIFY(l2.contains(QStringLiteral("Cwd")));

        // Interface group: shows the interface + aggregates, no process fields.
        Connection eth; eth.iface = QStringLiteral("eth0"); eth.ifIndex = 2;
        const QStringList li = labels(groupDetailRows(GroupBy::Interface, eth,
                                                      0, 0, 0, 0, 3, nullptr));
        QVERIFY(li.contains(QStringLiteral("Interface")));
        QVERIFY(!li.contains(QStringLiteral("PID")));
    }

    // orderedGroupIndices: the group-display-order policy behind
    // m_sortWithinGroups (default vs classic).
    void groupOrderingPolicy()
    {
        // Three groups. minSrc = first-appearance position (stable, source
        // order). Aggregates differ so the two policies diverge.
        QList<GroupSummary> g;
        g << GroupSummary{ 10.0, 0.0, 100, 0, /*minSrc*/0, QStringLiteral("eth0")  };
        g << GroupSummary{500.0, 0.0, 700, 0, /*minSrc*/1, QStringLiteral("wlan0") };
        g << GroupSummary{ 70.0, 0.0, 300, 0, /*minSrc*/2, QStringLiteral("lo")    };

        const int rxRateCol = 1;

        // sortWithinGroups (default): group order FROZEN at first-appearance
        // (minSrc) regardless of sort column/direction.
        QCOMPARE(orderedGroupIndices(g, rxRateCol, /*desc*/true,  /*within*/true),
                 (QList<int>{0, 1, 2}));
        QCOMPARE(orderedGroupIndices(g, rxRateCol, /*desc*/false, /*within*/true),
                 (QList<int>{0, 1, 2}));

        // Classic: order groups by the sort column's aggregate.
        // RX rate desc → wlan0(500), lo(70), eth0(10).
        QCOMPARE(orderedGroupIndices(g, rxRateCol, /*desc*/true,  /*within*/false),
                 (QList<int>{1, 2, 0}));
        // RX rate asc → eth0(10), lo(70), wlan0(500).
        QCOMPARE(orderedGroupIndices(g, rxRateCol, /*desc*/false, /*within*/false),
                 (QList<int>{0, 2, 1}));

        // Classic on the Flow column (0) orders by label (case-insensitive).
        // asc → eth0, lo, wlan0.
        QCOMPARE(orderedGroupIndices(g, /*Flow*/0, /*desc*/false, /*within*/false),
                 (QList<int>{0, 2, 1}));
    }

    // Equal aggregates fall back to the stable minSrc tiebreak (no shuffling).
    void groupOrderingStableTiebreak()
    {
        QList<GroupSummary> g;
        g << GroupSummary{100.0, 0.0, 0, 0, 0, QStringLiteral("a")};
        g << GroupSummary{100.0, 0.0, 0, 0, 1, QStringLiteral("b")};
        g << GroupSummary{100.0, 0.0, 0, 0, 2, QStringLiteral("c")};
        // All equal on RX rate → keep first-appearance order in both directions.
        QCOMPARE(orderedGroupIndices(g, 1, true,  false), (QList<int>{0, 1, 2}));
        QCOMPARE(orderedGroupIndices(g, 1, false, false), (QList<int>{0, 1, 2}));
    }

    void detailRows()
    {
        // Interface detail: a label/value row per field; optional fields still
        // listed with an em-dash (the bug fix — they used to render blank/"…").
        const QList<SettingRow> ifl =
            interfaceDetailRows(ifaceRow(QStringLiteral("eth0"), 0, 0, true));
        QVERIFY(ifl.size() >= 8);
        const auto hasLabel = [](const QList<SettingRow> &rs, const QString &l) {
            return std::any_of(rs.cbegin(), rs.cend(),
                               [&](const SettingRow &r){ return r.label == l; });
        };
        QVERIFY(hasLabel(ifl, QStringLiteral("MTU")));
        QVERIFY(hasLabel(ifl, QStringLiteral("State")));
        for (const SettingRow &r : ifl) {           // no blank values
            QVERIFY(!r.label.isEmpty());
            QVERIFY(!r.value.isEmpty());
        }

        // Connection detail includes the 5-tuple, direction AND process/
        // container rows (shown in every view mode, flat included).
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        Connection c;
        c.proto = L4Proto::Tcp;
        c.local.address  = QHostAddress(QStringLiteral("10.0.0.1")); c.local.port = 5000;
        c.remote.address = QHostAddress(QStringLiteral("1.1.1.1"));  c.remote.port = 443;
        c.direction = Direction::Outbound;
        c.process = {4242, QStringLiteral("curl"), {}, {}, 1000};
        c.container = {QStringLiteral("docker"), QStringLiteral("abc123"), QStringLiteral("web")};
        agg.updateConnections({c});
        const QList<SettingRow> cl = connectionDetailRows(agg, agg.rowAt(0));
        QVERIFY(hasLabel(cl, QStringLiteral("Direction")));
        QVERIFY(hasLabel(cl, QStringLiteral("Process")));
        QVERIFY(hasLabel(cl, QStringLiteral("Container")));

        // Stable keys: interface = name, connection = 5-tuple key().
        QCOMPARE(interfaceKey(ifaceRow(QStringLiteral("wlan0"), 0, 0)), QStringLiteral("wlan0"));
        QCOMPARE(connectionKey(agg.rowAt(0)), agg.rowAt(0).current.key());
    }

    void expansionStateIsKeyedAndReusable()
    {
        // The reusable inline-expand pattern (parked for future hierarchical
        // views): keyed by identity, toggles, bounded by retainOnly.
        ExpansionState ex;
        QVERIFY(ex.isEmpty());
        QVERIFY(!ex.isExpanded(QStringLiteral("a")));
        QVERIFY(ex.toggle(QStringLiteral("a")));        // returns new state (open)
        QVERIFY(ex.isExpanded(QStringLiteral("a")));
        QVERIFY(!ex.toggle(QStringLiteral("a")));       // back to collapsed
        QVERIFY(ex.apply(QStringLiteral("b"), +1));     // explicit expand
        QVERIFY(ex.apply(QStringLiteral("c"), +1));
        QCOMPARE(ex.count(), 2);
        QVERIFY(!ex.apply(QStringLiteral("b"), -1));    // explicit collapse
        QCOMPARE(ex.count(), 1);                        // only "c" remains
        ex.expand(QStringLiteral("gone"));
        ex.retainOnly({QStringLiteral("c")});           // drop keys not present
        QVERIFY(ex.isExpanded(QStringLiteral("c")));
        QVERIFY(!ex.isExpanded(QStringLiteral("gone")));
        QCOMPARE(ex.count(), 1);
    }
};

QTEST_MAIN(TestTuiFormat)
#include "test_tui_format.moc"
