#include "BenchData.h"
#include "aggregate/ConnectionAggregator.h"

#include <QtTest/QtTest>

using qiftop::aggregate::ConnectionAggregator;

namespace {

[[nodiscard]] QList<Connection> makeUdpPeerAggregationFlows(qsizetype count, int tick)
{
    qiftop::bench::FlowOptions options;
    options.count = count;
    options.tick = tick;
    options.udpRatio = 1.0;
    options.ipv6Ratio = 0.0;
    options.attributedRatio = 0.50;
    options.containerRatio = 0.25;
    QList<Connection> flows = qiftop::bench::makeConnections(options);

    const qsizetype peerCount = std::max<qsizetype>(1, count / 32);
    for (qsizetype i = 0; i < flows.size(); ++i) {
        const qsizetype peer = i % peerCount;
        Connection &c = flows[i];
        c.proto = L4Proto::Udp;
        c.direction = Direction::Outbound;
        c.local.address = QHostAddress(QStringLiteral("10.0.0.1"));
        c.local.port = quint16(20'000 + (i % 40'000));
        c.remote.address = QHostAddress(QStringLiteral("8.8.%1.%2")
            .arg((peer / 254) % 254)
            .arg((peer % 254) + 1));
        c.remote.port = quint16((peer % 2 == 0) ? 53 : 443);
    }
    return flows;
}

void addSizeRows(std::initializer_list<qsizetype> sizes, const char *suffix)
{
    for (qsizetype size : sizes) {
        const bool once = size >= qiftop::bench::kSize100K;
        QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(size).arg(QLatin1String(suffix))))
            << int(size) << once;
    }
}

} // namespace

class BenchConnectionAggregator : public QObject {
    Q_OBJECT

private slots:
    void updateExistingNoDnsNoUdp_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSize1K, qiftop::bench::kSizeCap,
                     qiftop::bench::kSize10K, qiftop::bench::kSize100K},
                    "no_dns/no_udp");
    }

    void updateExistingNoDnsNoUdp()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        options.udpRatio = 0.0;
        options.tick = 0;
        const QList<Connection> tick0 = qiftop::bench::makeConnections(options);
        const QList<Connection> tick1 = qiftop::bench::bumpCounters(tick0);

        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(false);
        agg.setRateSmoothingMs(0);
        agg.updateConnections(tick0);

        if (once) {
            QBENCHMARK_ONCE {
                agg.updateConnections(tick1);
            }
        } else {
            QBENCHMARK {
                agg.updateConnections(tick1);
            }
        }
        QCOMPARE(agg.rowCount(), count);
    }

    void updateExistingSmoothing_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K}, "smoothing");
    }

    void updateExistingSmoothing()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        options.udpRatio = 0.0;
        const QList<Connection> tick0 = qiftop::bench::makeConnections(options);
        const QList<Connection> tick1 = qiftop::bench::bumpCounters(tick0);

        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(false);
        agg.setRateSmoothingMs(750);
        agg.setPollIntervalMs(1000);
        agg.updateConnections(tick0);

        if (once) {
            QBENCHMARK_ONCE {
                agg.updateConnections(tick1);
            }
        } else {
            QBENCHMARK {
                agg.updateConnections(tick1);
            }
        }
        QCOMPARE(agg.rowCount(), count);
    }

    void updateUdpPeerAggregation_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K}, "udp_peer_aggregation");
    }

    void updateUdpPeerAggregation()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        const QList<Connection> tick0 = makeUdpPeerAggregationFlows(count, 0);
        const QList<Connection> tick1 = qiftop::bench::bumpCounters(tick0);

        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(true);
        agg.setRateSmoothingMs(0);
        agg.updateConnections(tick0);

        if (once) {
            QBENCHMARK_ONCE {
                agg.updateConnections(tick1);
            }
        } else {
            QBENCHMARK {
                agg.updateConnections(tick1);
            }
        }
        QVERIFY(agg.rowCount() < count);
    }

    void advanceSmoothing_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K}, "advance_smoothing");
    }

    void advanceSmoothing()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        options.udpRatio = 0.0;
        const QList<Connection> tick0 = qiftop::bench::makeConnections(options);
        const QList<Connection> tick1 = qiftop::bench::bumpCounters(tick0);

        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(false);
        agg.setRateSmoothingMs(750);
        agg.setPollIntervalMs(1000);
        agg.updateConnections(tick0);
        agg.updateConnections(tick1);

        if (once) {
            QBENCHMARK_ONCE {
                agg.advanceSmoothing();
            }
        } else {
            QBENCHMARK {
                agg.advanceSmoothing();
            }
        }
        QCOMPARE(agg.rowCount(), count);
    }

    void stalePrune()
    {
        qiftop::bench::FlowOptions options;
        options.count = qiftop::bench::kSizeCap;
        options.udpRatio = 0.0;
        const QList<Connection> tick0 = qiftop::bench::makeConnections(options);

        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(false);
        agg.setStaleRetentionMs(0);
        agg.updateConnections(tick0);
        QTest::qWait(2);

        QBENCHMARK_ONCE {
            agg.updateConnections({});
        }
        QCOMPARE(agg.rowCount(), 0);
    }
};

QTEST_MAIN(BenchConnectionAggregator)
#include "bench_connection_aggregator.moc"
