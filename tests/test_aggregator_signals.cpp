// Signal-coalescing tests for the plain-QObject aggregators.

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>
#include <QVector>

#include <cstdio>
#include <utility>

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/Connection.h"

using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;

namespace {

struct Range {
    int first = -1;
    int last = -1;
};

Connection makeFlow(int id, quint64 rx, quint64 tx)
{
    Connection c;
    c.proto = L4Proto::Tcp;
    c.local.address = QHostAddress(QStringLiteral("10.0.0.%1").arg(id + 1));
    c.local.port = quint16(40000 + id);
    c.remote.address = QHostAddress(QStringLiteral("203.0.113.%1").arg(id + 1));
    c.remote.port = 443;
    c.rxBytes = rx;
    c.txBytes = tx;
    c.direction = Direction::Outbound;
    return c;
}

QList<Connection> makeFlows(int count, quint64 rxBase, quint64 txBase)
{
    QList<Connection> flows;
    flows.reserve(count);
    for (int i = 0; i < count; ++i)
        flows.append(makeFlow(i, rxBase + quint64(i * 1000), txBase + quint64(i * 500)));
    return flows;
}

InterfaceStats makeIface(QString name, quint64 rx, quint64 tx)
{
    InterfaceStats s;
    s.name = std::move(name);
    s.rxBytes = rx;
    s.txBytes = tx;
    s.isUp = true;
    return s;
}

QList<Range> rangesFrom(const QSignalSpy &spy)
{
    QList<Range> ranges;
    ranges.reserve(spy.count());
    for (const QList<QVariant> &args : spy)
        ranges.append({args.at(0).toInt(), args.at(1).toInt()});
    return ranges;
}

bool expect(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool expectTouchedExactly(const QList<Range> &ranges, int rowCount)
{
    QVector<bool> touched(rowCount, false);
    for (const Range &r : ranges) {
        if (!expect(r.first >= 0 && r.last >= r.first && r.last < rowCount,
                    "range bounds are valid")) {
            return false;
        }
        for (int row = r.first; row <= r.last; ++row) {
            if (!expect(!touched[row], "ranges do not overlap"))
                return false;
            touched[row] = true;
        }
    }
    for (int row = 0; row < rowCount; ++row) {
        if (!expect(touched[row], "every touched row is covered"))
            return false;
    }
    return true;
}

bool allExistingConnectionRowsUpdateAsOneRange()
{
    ConnectionAggregator agg;
    agg.setUdpAggregateByPeer(false);
    agg.setRateSmoothingMs(0);
    agg.updateConnections(makeFlows(5, 1000, 500));
    QTest::qWait(20);

    QSignalSpy updates(&agg, &ConnectionAggregator::rowsUpdated);
    agg.updateConnections(makeFlows(5, 100000, 50000));

    const QList<Range> ranges = rangesFrom(updates);
    return expect(agg.rowCount() == 5, "five rows remain")
        && expect(updates.count() == 1, "contiguous refreshed rows emit one range")
        && expect(updates.count() < agg.rowCount(), "emission count is less than row count")
        && expectTouchedExactly(ranges, agg.rowCount())
        && expect(ranges.at(0).first == 0 && ranges.at(0).last == agg.rowCount() - 1,
                  "refreshed range spans all rows");
}

bool newlyStaleRowsUpdateAsOneRange()
{
    ConnectionAggregator agg;
    agg.setUdpAggregateByPeer(false);
    agg.setStaleRetentionMs(60'000);
    agg.updateConnections(makeFlows(4, 1000, 500));
    QTest::qWait(5);

    QSignalSpy updates(&agg, &ConnectionAggregator::rowsUpdated);
    agg.updateConnections({});

    const QList<Range> ranges = rangesFrom(updates);
    return expect(agg.rowCount() == 4, "stale rows are retained")
        && expect(updates.count() == 1, "contiguous stale rows emit one range")
        && expect(updates.count() < agg.rowCount(), "stale emission count is less than row count")
        && expectTouchedExactly(ranges, agg.rowCount())
        && expect(ranges.at(0).first == 0 && ranges.at(0).last == agg.rowCount() - 1,
                  "stale range spans all rows");
}

bool discontiguousExistingRowsEmitMaximalRuns()
{
    ConnectionAggregator agg;
    agg.setUdpAggregateByPeer(false);
    agg.setStaleRetentionMs(60'000);
    agg.setRateSmoothingMs(0);
    agg.updateConnections(makeFlows(5, 1000, 500));
    QTest::qWait(5);
    agg.updateConnections({});

    QList<Connection> refreshed;
    for (int row : {1, 2, 4}) {
        Connection c = agg.rowAt(row).current;
        c.rxBytes += 50'000;
        c.txBytes += 25'000;
        refreshed.append(c);
    }

    QSignalSpy updates(&agg, &ConnectionAggregator::rowsUpdated);
    agg.updateConnections(refreshed);

    const QList<Range> ranges = rangesFrom(updates);
    return expect(agg.rowCount() == 5, "five rows remain after sparse refresh")
        && expect(updates.count() == 2, "sparse refreshed rows emit maximal runs")
        && expect(ranges.at(0).first == 1 && ranges.at(0).last == 2,
                  "first sparse run is rows 1..2")
        && expect(ranges.at(1).first == 4 && ranges.at(1).last == 4,
                  "second sparse run is row 4");
}

bool smoothingRowsUpdateAsOneRange()
{
    ConnectionAggregator agg;
    agg.setUdpAggregateByPeer(false);
    agg.setRateSmoothingMs(500);
    agg.setPollIntervalMs(100);
    agg.updateConnections(makeFlows(4, 0, 0));
    QTest::qWait(20);
    agg.updateConnections(makeFlows(4, 1'000'000, 500'000));
    QTest::qWait(30);

    QSignalSpy updates(&agg, &ConnectionAggregator::rowsUpdated);
    agg.advanceSmoothing();

    const QList<Range> ranges = rangesFrom(updates);
    return expect(agg.rowCount() == 4, "four smoothing rows remain")
        && expect(updates.count() == 1, "contiguous smoothing rows emit one range")
        && expect(updates.count() < agg.rowCount(), "smoothing emission count is less than row count")
        && expectTouchedExactly(ranges, agg.rowCount())
        && expect(ranges.at(0).first == 0 && ranges.at(0).last == agg.rowCount() - 1,
                  "smoothing range spans all rows");
}

bool interfaceAggregatorAlreadyBatchesAllRows()
{
    InterfaceAggregator agg;
    agg.updateStats({
        makeIface(QStringLiteral("eth0"), 1000, 500),
        makeIface(QStringLiteral("wlan0"), 2000, 1000),
    });
    QTest::qWait(20);

    QSignalSpy updates(&agg, &InterfaceAggregator::rowsChanged);
    agg.updateStats({
        makeIface(QStringLiteral("eth0"), 3000, 1500),
        makeIface(QStringLiteral("wlan0"), 5000, 2500),
    });

    const QList<Range> ranges = rangesFrom(updates);
    return expect(agg.rowCount() == 2, "two interface rows remain")
        && expect(updates.count() == 1, "interface rowsChanged is already batched")
        && expectTouchedExactly(ranges, agg.rowCount())
        && expect(ranges.at(0).first == 0 && ranges.at(0).last == agg.rowCount() - 1,
                  "interface range spans all rows");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    bool ok = true;
    ok = allExistingConnectionRowsUpdateAsOneRange() && ok;
    ok = newlyStaleRowsUpdateAsOneRange() && ok;
    ok = discontiguousExistingRowsEmitMaximalRuns() && ok;
    ok = smoothingRowsUpdateAsOneRange() && ok;
    ok = interfaceAggregatorAlreadyBatchesAllRows() && ok;

    return ok ? 0 : 1;
}
