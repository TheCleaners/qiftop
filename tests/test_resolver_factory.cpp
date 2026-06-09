// Smoke-tests for createProcessResolver(). The factory must:
//   * Always return a non-null, initialise()-d resolver.
//   * Never throw or assert regardless of the config knobs.
//   * In step 1 specifically: return a resolver advertising no caps
//     (we ship Null until step 2 lands the Linux composite).

#include <QTest>

#include "backend/ProcessResolverFactory.h"

using namespace qiftop::backend;

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
};

QTEST_GUILESS_MAIN(TestResolverFactory)
#include "test_resolver_factory.moc"
