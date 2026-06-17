#include "BenchData.h"
#include "util/ConnectionFilter.h"

#include <QtTest/QtTest>

namespace {

struct Hostnames {
    QString local;
    QString remote;
};

[[nodiscard]] QList<Hostnames> makeHostnames(qsizetype count)
{
    QList<Hostnames> names;
    names.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        names.append(Hostnames{
            QStringLiteral("client-%1.lan").arg(i % 2048),
            (i % 2 == 0)
                ? QStringLiteral("api-%1.example.net").arg(i % 4096)
                : QStringLiteral("peer-%1.invalid").arg(i % 4096),
        });
    }
    return names;
}

[[nodiscard]] qsizetype countMatches(const qiftop::filter::ExprPtr &expr,
                                     const QList<Connection> &connections,
                                     const QList<Hostnames> *hostnames)
{
    qsizetype matched = 0;
    for (qsizetype i = 0; i < connections.size(); ++i) {
        const Hostnames empty;
        const Hostnames &names = hostnames ? hostnames->at(i) : empty;
        const qiftop::filter::Context ctx{
            connections[i],
            double((i % 2000) * 1024),
            double((i % 1500) * 512),
            names.local,
            names.remote,
        };
        if (qiftop::filter::matches(expr, ctx))
            ++matched;
    }
    return matched;
}

void addMatchRows(const QString &expression, const char *name, bool hostnames)
{
    QTest::newRow(qPrintable(QStringLiteral("%1/4096").arg(QLatin1String(name))))
        << expression << int(qiftop::bench::kSizeCap) << hostnames << false;
    QTest::newRow(qPrintable(QStringLiteral("%1/100000").arg(QLatin1String(name))))
        << expression << int(qiftop::bench::kSize100K) << hostnames << true;
}

} // namespace

class BenchFilter : public QObject {
    Q_OBJECT

private slots:
    void matchExpressions_data()
    {
        QTest::addColumn<QString>("expression");
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("withHostnames");
        QTest::addColumn<bool>("once");

        addMatchRows(QStringLiteral("bytes > 10Mi and proto:tcp"), "bytes_proto", false);
        addMatchRows(QStringLiteral("host:example"), "host_example", true);
        addMatchRows(QStringLiteral("container:docker or chain_has:kubernetes"),
                     "container_or_chain", false);
        addMatchRows(QStringLiteral("comm~\"(nginx|ssh|curl)\""), "comm_regex", false);
    }

    void matchExpressions()
    {
        QFETCH(QString, expression);
        QFETCH(int, count);
        QFETCH(bool, withHostnames);
        QFETCH(bool, once);

        qiftop::bench::FlowOptions options;
        options.count = count;
        options.containerRatio = 0.60;
        options.includeContainerChains = true;
        const QList<Connection> connections = qiftop::bench::makeConnections(options);
        const QList<Hostnames> hostnames = withHostnames ? makeHostnames(count) : QList<Hostnames>{};

        const auto parsed = qiftop::filter::parse(expression);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QVERIFY(parsed.expr != nullptr);

        if (once) {
            QBENCHMARK_ONCE {
                m_lastMatches = countMatches(parsed.expr, connections,
                                             withHostnames ? &hostnames : nullptr);
            }
        } else {
            QBENCHMARK {
                m_lastMatches = countMatches(parsed.expr, connections,
                                             withHostnames ? &hostnames : nullptr);
            }
        }
        QVERIFY(m_lastMatches >= 0);
    }

    void parseLongExpression()
    {
        const QString expression = QStringLiteral(
            "(proto:tcp and (bytes > 1Mi or rate > 10K)) and "
            "(host:example or iface:eth or comm~\"(nginx|ssh|curl)\") and "
            "(container:docker or runtime:podman or chain_has:kubernetes) and "
            "not reason:forwarded");

        QBENCHMARK {
            for (int i = 0; i < 100; ++i) {
                const auto parsed = qiftop::filter::parse(expression);
                m_lastParseOk = parsed.error.isEmpty() && parsed.expr != nullptr;
            }
        }
        QVERIFY(m_lastParseOk);
    }

private:
    qsizetype m_lastMatches = 0;
    bool m_lastParseOk = false;
};

QTEST_APPLESS_MAIN(BenchFilter)
#include "bench_filter.moc"
