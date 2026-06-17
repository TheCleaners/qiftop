#include "BenchData.h"
#include "backend/linux/FlowTopK.h"

#include <QtTest/QtTest>

#include <algorithm>

namespace {

enum class Distribution {
    Increasing = 0,
    Decreasing,
    Random,
    Ties,
};

[[nodiscard]] QString distributionName(Distribution d)
{
    switch (d) {
    case Distribution::Increasing: return QStringLiteral("increasing");
    case Distribution::Decreasing: return QStringLiteral("decreasing");
    case Distribution::Random:     return QStringLiteral("random");
    case Distribution::Ties:       return QStringLiteral("ties");
    }
    return QStringLiteral("unknown");
}

[[nodiscard]] QList<Connection> makeOffered(qsizetype count, Distribution distribution)
{
    qiftop::bench::FlowOptions options;
    options.count = count;
    options.udpRatio = 0.25;
    options.ipv6Ratio = 0.05;
    QList<Connection> flows = qiftop::bench::makeConnections(options);

    for (qsizetype i = 0; i < flows.size(); ++i) {
        quint64 total = 0;
        switch (distribution) {
        case Distribution::Increasing:
            total = quint64(i + 1) * 1024ULL;
            break;
        case Distribution::Decreasing:
            total = quint64(flows.size() - i) * 1024ULL;
            break;
        case Distribution::Random:
            total = quint64((i * 1'103'515'245ULL + 12'345ULL) & 0x0fffffffULL) * 64ULL;
            break;
        case Distribution::Ties:
            total = quint64((i % 128) + 1) * 4096ULL;
            break;
        }
        flows[i].rxBytes = total / 3ULL;
        flows[i].txBytes = total - flows[i].rxBytes;
    }
    return flows;
}

void addRowsFor(Distribution distribution)
{
    for (qsizetype count : {qsizetype(4'096), qsizetype(10'000),
                           qsizetype(100'000), qsizetype(250'000)}) {
        const bool once = count >= 100'000;
        QTest::newRow(qPrintable(QStringLiteral("%1/%2")
            .arg(distributionName(distribution))
            .arg(count)))
            << int(count) << int(distribution) << once;
    }
}

} // namespace

class BenchFlowTopK : public QObject {
    Q_OBJECT

private slots:
    void admit_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<int>("distributionValue");
        QTest::addColumn<bool>("once");

        addRowsFor(Distribution::Increasing);
        addRowsFor(Distribution::Decreasing);
        addRowsFor(Distribution::Random);
        addRowsFor(Distribution::Ties);
    }

    void admit()
    {
        QFETCH(int, count);
        QFETCH(int, distributionValue);
        QFETCH(bool, once);
        constexpr int kCap = 4096;

        const auto distribution = static_cast<Distribution>(distributionValue);
        const QList<Connection> offered = makeOffered(count, distribution);

        if (once) {
            QBENCHMARK_ONCE {
                QList<Connection> heap;
                heap.reserve(kCap);
                for (const Connection &c : offered)
                    qiftop::backend::linux::admitFlowTopK(heap, c, kCap);
                m_lastHeapSize = heap.size();
                m_lastFrontBytes = qiftop::backend::linux::flowBytesTotal(heap.constFirst());
            }
        } else {
            QBENCHMARK {
                QList<Connection> heap;
                heap.reserve(kCap);
                for (const Connection &c : offered)
                    qiftop::backend::linux::admitFlowTopK(heap, c, kCap);
                m_lastHeapSize = heap.size();
                m_lastFrontBytes = qiftop::backend::linux::flowBytesTotal(heap.constFirst());
            }
        }
        QCOMPARE(m_lastHeapSize, std::min(count, kCap));
        QVERIFY(m_lastFrontBytes > 0);
    }

private:
    int m_lastHeapSize = 0;
    quint64 m_lastFrontBytes = 0;
};

QTEST_APPLESS_MAIN(BenchFlowTopK)
#include "bench_flow_topk.moc"
