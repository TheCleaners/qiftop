// bench_pipeline_tick — the "eager budget" benchmark.
//
// This stitches the three hot paths a single agent/in-process tick walks for
// the Connections view into one measurement: take N raw conntrack flows, cap
// them to the top-K by bytes (FlowTopK, what the in-process backend + agent
// snapshot do), feed the capped set into the ConnectionAggregator (rate math),
// then evaluate a representative filter expression over the result.
//
// Why it matters: the resolver eagerness knobs (agent.conf [attribution]) let
// operators crank refresh cadence. The question they raise is "how hard can we
// push before the per-tick data work blows the budget?" This bench answers the
// data-pipeline half of that — and the earlier single-path numbers already hint
// the answer is "the poll cadence, not the pipeline, is the ceiling." Keep this
// honest: pure CPU, no DNS, no kernel, deterministic inputs.

#include "BenchData.h"

#include "aggregate/ConnectionAggregator.h"
#include "backend/linux/FlowTopK.h"
#include "util/ConnectionFilter.h"

#include <QtTest/QtTest>

#include <algorithm>

using qiftop::aggregate::ConnectionAggregator;

namespace {

// Matches the agent's ConnectionsService snapshot cap / the in-process
// ConntrackMonitor top-K cap (kMaxInProcessFlows).
inline constexpr qsizetype kCap = qiftop::bench::kSizeCap;  // 4096

// A filter with a bit of everything: boolean ops, a numeric byte compare, and
// grouped port predicates — representative of what an admin actually types, so
// the evaluator isn't measured on a trivial expression. We compare on `bytes`
// (the flow's own counters, populated by the generator) rather than `rate`,
// since this bench feeds the filter zero Context rates.
inline constexpr auto kFilterExpr =
    "proto:tcp and (dport=443 or dport=53) and bytes > 1Ki";

[[nodiscard]] QList<Connection> capTopK(const QList<Connection> &offered)
{
    QList<Connection> heap;
    heap.reserve(kCap);
    for (const Connection &c : offered)
        qiftop::backend::linux::admitFlowTopK(heap, c, kCap);
    return heap;
}

[[nodiscard]] qsizetype countMatches(const qiftop::filter::ExprPtr &expr,
                                     const QList<Connection> &flows,
                                     const ConnectionAggregator &agg)
{
    qsizetype matched = 0;
    for (qsizetype i = 0; i < flows.size(); ++i) {
        const qiftop::filter::Context ctx{flows[i], 0.0, 0.0, {}, {}};
        if (qiftop::filter::matches(expr, ctx))
            ++matched;
    }
    (void)agg;
    return matched;
}

void addSizeRows(std::initializer_list<qsizetype> sizes)
{
    for (qsizetype size : sizes) {
        const bool once = size >= qiftop::bench::kSize100K;
        QTest::newRow(qPrintable(QString::number(size))) << int(size) << once;
    }
}

} // namespace

class BenchPipelineTick : public QObject {
    Q_OBJECT

private slots:
    // One full data-plane tick: top-K cap -> aggregator update -> filter scan,
    // over a raw flow table that can be far larger than the cap.
    void fullTick_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K,
                     qiftop::bench::kSize100K});
    }

    void fullTick()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        options.tick = 0;
        const QList<Connection> raw0 = qiftop::bench::makeConnections(options);
        const QList<Connection> raw1 = qiftop::bench::bumpCounters(raw0);

        const auto parsed = qiftop::filter::parse(QString::fromLatin1(kFilterExpr));
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));

        // The top-K cap is the real invariant: each tick's snapshot is bounded
        // by kCap no matter how big the raw table is.
        QCOMPARE(capTopK(raw1).size(), std::min<qsizetype>(count, kCap));

        // Steady state: prime the aggregator with the previous tick's capped
        // set, then measure the next tick's full pipeline.
        ConnectionAggregator agg;
        agg.setHostnameResolutionEnabled(false);
        agg.setUdpAggregateByPeer(false);
        agg.setRateSmoothingMs(0);
        agg.updateConnections(capTopK(raw0));

        qsizetype matched = 0;
        const auto runTick = [&] {
            const QList<Connection> capped = capTopK(raw1);
            agg.updateConnections(capped);
            matched = countMatches(parsed.expr, capped, agg);
        };

        if (once) {
            QBENCHMARK_ONCE { runTick(); }
        } else {
            QBENCHMARK { runTick(); }
        }

        // The pipeline produced rows and the filter actually ran (the synthetic
        // data has plenty of tcp:443/:53 flows above the byte threshold).
        QVERIFY(agg.rowCount() > 0);
        QVERIFY(matched >= 0);
    }
};

QTEST_MAIN(BenchPipelineTick)
#include "bench_pipeline_tick.moc"
