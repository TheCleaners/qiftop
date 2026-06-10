// Smoke-tests for createProcessResolver(). The factory must:
//   * Always return a non-null, initialise()-d resolver.
//   * Never throw or assert regardless of the config knobs.
//   * In step 1 specifically: return a resolver advertising no caps
//     (we ship Null until step 2 lands the Linux composite).

#include <QTest>

#include "backend/Connection.h"
#include "backend/NetworkMonitor.h"
#include "backend/ProcessResolver.h"
#include "backend/ProcessResolverFactory.h"
#include "agent/InterfacesService.h"

#include <utility>

using namespace qiftop::backend;

namespace {

class FakeNetworkMonitor final : public NetworkMonitor {
public:
    void start() override {}
    void stop() override {}
};

class FakeResolver final : public ProcessResolver {
public:
    explicit FakeResolver(QStringList caps) : m_caps(std::move(caps)) {}

    bool initialize() override { return true; }
    QStringList capabilities() const override { return m_caps; }
    qint32 resolvePid(const Connection &) override { return 0; }
    std::optional<ProcessInfo> enrichPid(qint32) override { return std::nullopt; }
    std::optional<ContainerInfo> resolveContainerForPid(qint32) override { return std::nullopt; }

private:
    QStringList m_caps;
};

} // namespace

class TestResolverFactory : public QObject {
    Q_OBJECT
private slots:
    void defaultConfigReturnsUsableResolver()
    {
        auto r = createProcessResolver();
        QVERIFY(r);
        // Step 1: factory always returns Null regardless of compile flags.
        QVERIFY(r->capabilities().isEmpty());
        QVERIFY(!r->resolveFlow(Connection{}).has_value());
        QVERIFY(!r->resolveContainerForPid(1).has_value());
    }

    void allFeaturesOffReturnsUsableResolver()
    {
        ProcessResolverConfig cfg;
        cfg.processAttribution   = false;
        cfg.containerAttribution = false;
        cfg.netnsScan            = false;
        auto r = createProcessResolver(cfg);
        QVERIFY(r);
        QVERIFY(r->capabilities().isEmpty());
    }

    void containerWireCapabilities_data()
    {
        QTest::addColumn<QStringList>("resolverCaps");
        QTest::addColumn<bool>("expectChainWire");

        QTest::newRow("leaf-only")
            << QStringList{QStringLiteral("container-attribution")}
            << false;
        QTest::newRow("leaf-and-chain")
            << QStringList{QStringLiteral("container-attribution"),
                           QStringLiteral("container-chain")}
            << true;
    }

    void containerWireCapabilities()
    {
        QFETCH(QStringList, resolverCaps);
        QFETCH(bool, expectChainWire);

        FakeNetworkMonitor monitor;
        FakeResolver resolver(resolverCaps);
        qiftop::agent::InterfacesService service(&monitor);
        service.setProcessResolver(&resolver);

        const QStringList caps = service.capabilities();
        QVERIFY(caps.contains(QStringLiteral("container-attribution-wire")));
        QCOMPARE(caps.contains(QStringLiteral("container-chain-wire")), expectChainWire);
    }
};

QTEST_GUILESS_MAIN(TestResolverFactory)
#include "test_resolver_factory.moc"
