// Sanity tests for NullProcessResolver — the universal no-op fallback.
// These pin the contract that drives the "missing capability ⇒ hide UI"
// rule: an inert resolver MUST advertise no tokens and MUST resolve
// nothing, no matter what's thrown at it.

#include <QTest>

#include "backend/Connection.h"
#include "backend/null/NullProcessResolver.h"

using namespace qiftop::backend;

class TestNullProcessResolver : public QObject {
    Q_OBJECT
private slots:
    void initializeAlwaysSucceeds()
    {
        NullProcessResolver r;
        QVERIFY(r.initialize());
    }

    void capabilitiesAreEmpty()
    {
        NullProcessResolver r;
        QVERIFY(r.capabilities().isEmpty());
    }

    void resolveFlowReturnsNullopt()
    {
        NullProcessResolver r;
        Connection c{};
        QVERIFY(!r.resolveFlow(c).has_value());
    }

    void resolveContainerReturnsNullopt()
    {
        NullProcessResolver r;
        QVERIFY(!r.resolveContainerForPid(1).has_value());
        QVERIFY(!r.resolveContainerForPid(0).has_value());
    }
};

QTEST_GUILESS_MAIN(TestNullProcessResolver)
#include "test_process_resolver_null.moc"
