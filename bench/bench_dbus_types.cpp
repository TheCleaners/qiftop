// bench_dbus_types — wire conversion + marshalling cost for the connection
// snapshot path.
//
// Every agent tick converts its Connection rows to ConnectionDto (toDtos) and
// marshals them onto the bus; every client converts them back (fromDtos). These
// are OUR code (22 fields incl. the nested container chain) and scale with the
// snapshot size, so they're the baseline to watch before the v0.4 async
// AttributionChanged patch signal starts re-marshalling refined rows.
//
// We measure the bus-free, deterministic parts: the toDtos/fromDtos conversion
// layer. The actual QDBusArgument marshal/demarshal needs a live bus (a
// QDBusArgument can't even be marshalled into standalone in-process — it aborts;
// see tests/test_wire_compat.cpp), so the wire serialise/deserialise cost is an
// integration-tier concern, not a pure microbench.

#include "BenchData.h"
#include "dbus/Types.h"

#include <QtTest/QtTest>

namespace {

void addSizeRows(std::initializer_list<qsizetype> sizes)
{
    for (qsizetype size : sizes) {
        const bool once = size >= qiftop::bench::kSize100K;
        QTest::newRow(qPrintable(QString::number(size))) << int(size) << once;
    }
}

} // namespace

class BenchDbusTypes : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Register the DTO metatypes so the QDBusArgument marshalling operators
        // are wired up (mirrors what the agent/clients do at startup).
        qiftop::dbus::registerTypes();
    }

    // Connection -> ConnectionDto: the agent's per-snapshot conversion.
    void toDtos_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K,
                     qiftop::bench::kSize100K});
    }

    void toDtos()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        const QList<Connection> flows = qiftop::bench::makeConnections(options);

        qsizetype produced = 0;
        const auto run = [&] { produced = qiftop::dbus::toDtos(flows).size(); };
        if (once) { QBENCHMARK_ONCE { run(); } }
        else      { QBENCHMARK      { run(); } }
        QCOMPARE(produced, qsizetype(count));
    }

    // ConnectionDto -> Connection: what every client does per snapshot.
    void fromDtos_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("once");
        addSizeRows({qiftop::bench::kSizeCap, qiftop::bench::kSize10K,
                     qiftop::bench::kSize100K});
    }

    void fromDtos()
    {
        QFETCH(int, count);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        const qiftop::dbus::ConnectionDtoList dtos =
            qiftop::dbus::toDtos(qiftop::bench::makeConnections(options));

        qsizetype produced = 0;
        const auto run = [&] { produced = qiftop::dbus::fromDtos(dtos).size(); };
        if (once) { QBENCHMARK_ONCE { run(); } }
        else      { QBENCHMARK      { run(); } }
        QCOMPARE(produced, qsizetype(count));
    }
};

QTEST_MAIN(BenchDbusTypes)
#include "bench_dbus_types.moc"
