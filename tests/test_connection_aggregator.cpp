// Unit tests for ConnectionAggregator — the plain-QObject flow-table
// aggregator (rate smoothing, UDP peer aggregation, direction, stale
// retention) extracted from ConnectionModel into libqiftop. No
// QAbstractItemModel, no Widgets.

#include <QSignalSpy>
#include <QTest>

#include "aggregate/ConnectionAggregator.h"
#include "backend/Connection.h"

using qiftop::aggregate::ConnectionAggregator;

namespace {

Connection makeFlow(L4Proto p, const char *l, quint16 lp,
                    const char *r, quint16 rp,
                    quint64 rx, quint64 tx,
                    Direction dir = Direction::Outbound)
{
    Connection c;
    c.proto          = p;
    c.local.address  = QHostAddress(QString::fromLatin1(l));
    c.local.port     = lp;
    c.remote.address = QHostAddress(QString::fromLatin1(r));
    c.remote.port    = rp;
    c.rxBytes        = rx;
    c.txBytes        = tx;
    c.direction      = dir;
    return c;
}

} // namespace

class TestConnectionAggregator : public QObject {
    Q_OBJECT

private slots:
    void firstSnapshotInsertsRowsZeroRate()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        QSignalSpy insSpy(&agg, &ConnectionAggregator::rowsInserted);

        agg.updateConnections({makeFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443, 1000, 500)});

        QCOMPARE(agg.rowCount(), 1);
        QCOMPARE(insSpy.count(), 1);
        QCOMPARE(agg.rowAt(0).rxRate, 0.0);   // no previous sample
        QCOMPARE(agg.rowAt(0).txRate, 0.0);
    }

    void secondSnapshotComputesRawRate()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        // smoothing off => display rate == raw rate immediately.
        agg.setRateSmoothingMs(0);
        agg.updateConnections({makeFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443, 1000, 500)});

        QSignalSpy updSpy(&agg, &ConnectionAggregator::rowsUpdated);
        QTest::qWait(50);
        agg.updateConnections({makeFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443, 9000, 4500)});

        QCOMPARE(agg.rowCount(), 1);
        QVERIFY(updSpy.count() >= 1);
        QVERIFY(agg.rowAt(0).rxRate > 0.0);
        // rx delta (8000) == 2 * tx delta (4000).
        QVERIFY(qFuzzyCompare(agg.rowAt(0).rxRate, 2.0 * agg.rowAt(0).txRate));
    }

    void disappearingFlowGoesStaleThenPruned()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        agg.setStaleRetentionMs(0);     // prune as soon as stale
        agg.updateConnections({makeFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443, 1000, 500)});
        QCOMPARE(agg.rowCount(), 1);

        QSignalSpy remSpy(&agg, &ConnectionAggregator::rowsRemoved);
        // Let time elapse so lastSeenMs is in the past, then deliver an empty
        // snapshot: the row is marked stale AND (retention 0, now > lastSeen)
        // pruned on the same tick.
        QTest::qWait(5);
        agg.updateConnections({});
        QCOMPARE(agg.rowCount(), 0);
        QCOMPARE(remSpy.count(), 1);
    }

    void udpAggregationCollapsesEphemeralPorts()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(true);
        // Two outbound UDP flows to the same peer from different ephemeral
        // local ports collapse into ONE aggregated row (local port -> "*").
        agg.updateConnections({
            makeFlow(L4Proto::Udp, "10.0.0.1", 40001, "8.8.8.8", 53, 100, 50, Direction::Outbound),
            makeFlow(L4Proto::Udp, "10.0.0.1", 40002, "8.8.8.8", 53, 200, 75, Direction::Outbound),
        });
        QCOMPARE(agg.rowCount(), 1);
        const Connection &c = agg.rowAt(0).current;
        QCOMPARE(c.local.port, quint16(0));            // masked ("*")
        QCOMPARE(c.remote.port, quint16(53));
        // Aggregate counters are the sum of the members.
        QCOMPARE(c.rxBytes, quint64(300));
        QCOMPARE(c.txBytes, quint64(125));
    }

    void udpAggregationDisabledKeepsSeparateRows()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        agg.updateConnections({
            makeFlow(L4Proto::Udp, "10.0.0.1", 40001, "8.8.8.8", 53, 100, 50, Direction::Outbound),
            makeFlow(L4Proto::Udp, "10.0.0.1", 40002, "8.8.8.8", 53, 200, 75, Direction::Outbound),
        });
        QCOMPARE(agg.rowCount(), 2);
    }

    void copyHelpersRenderEndpoints()
    {
        ConnectionAggregator agg;
        agg.setUdpAggregateByPeer(false);
        agg.updateConnections({makeFlow(L4Proto::Tcp, "10.0.0.1", 5000, "1.1.1.1", 443, 1, 1)});
        // No DNS => numeric. Source = local (outbound), dest = remote.
        QCOMPARE(agg.copyTextForEndpoint(0, ConnectionAggregator::FlowEnd::Source),
                 QStringLiteral("10.0.0.1:5000"));
        QCOMPARE(agg.copyTextForEndpoint(0, ConnectionAggregator::FlowEnd::Destination),
                 QStringLiteral("1.1.1.1:443"));
        QCOMPARE(agg.peerAddressText(0), QStringLiteral("1.1.1.1"));
    }
};

QTEST_MAIN(TestConnectionAggregator)
#include "test_connection_aggregator.moc"
