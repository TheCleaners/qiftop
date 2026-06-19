// Unit test for ResolverDeepWorker — the same-thread re-enrichment worker that
// retries weakly-attributed flows against the resolver on a coalescing timer
// and emits a refinement once attribution improves (v0.4 §5). Drives a real
// Qt event loop; uses a fake resolver that "finds" the PID only after a few
// attempts, simulating a socket landing in the resolver cache after refresh.

#include <QSignalSpy>
#include <QTest>

#include "backend/ProcessResolver.h"
#include "backend/ResolverDeepWorker.h"

using namespace qiftop::backend;

namespace {

// Returns pid=0 until the Nth resolvePid call, then a fixed pid — modelling a
// flow whose owning socket only becomes visible after a cache refresh.
class LateResolver final : public ProcessResolver {
public:
    explicit LateResolver(int attributeOnCall) : m_threshold(attributeOnCall) {}

    bool initialize() override { return true; }
    QStringList capabilities() const override
    {
        return {QStringLiteral("process-attribution")};
    }

    qint32 resolvePid(const Connection &) override
    {
        return (++m_calls >= m_threshold) ? 4242 : 0;
    }
    std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        ProcessInfo p;
        p.pid  = pid;
        p.uid  = 1000;
        p.comm = QStringLiteral("late");
        return p;
    }
    std::optional<ContainerInfo> resolveContainerForPid(qint32) override
    {
        return std::nullopt;
    }

    [[nodiscard]] int calls() const { return m_calls; }

private:
    int m_threshold;
    int m_calls = 0;
};

DeepAttributionRequest mkRequest()
{
    Connection c;
    c.proto          = L4Proto::Tcp;
    c.local.address  = QHostAddress(QStringLiteral("127.0.0.1"));
    c.local.port     = 40000;
    c.remote.address = QHostAddress(QStringLiteral("203.0.113.7"));
    c.remote.port    = 443;
    c.reason         = AttributionReason::NoLocalSocket;

    DeepAttributionRequest r;
    r.key  = keyOf(c);
    r.flow = c;
    return r;
}

// Fast budgets so the test doesn't wait on the 100 ms production cadence.
ResolverTuning fastTuning()
{
    ResolverTuning t;
    t.deepCoalesceMs  = 5;
    t.deepBatchMax    = 64;
    t.deepMaxAttempts = 20;
    return t;
}

} // namespace

class TestDeepWorker : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<QList<DeepAttributionUpdate>>();
    }

    void emitsRefinementOnceAttributionImproves()
    {
        LateResolver resolver(/*attributeOnCall=*/3); // miss, miss, then hit
        ResolverDeepWorker worker;
        worker.setResolver(&resolver);
        worker.setTuning(fastTuning());

        QSignalSpy spy(&worker, &DeepAttributionWorker::refined);
        QVERIFY(spy.isValid());

        worker.enqueue({mkRequest()});

        // The worker retries across coalesce ticks until the resolver finally
        // attributes the flow, then emits exactly one refinement and drains.
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);

        const auto updates =
            spy.takeFirst().at(0).value<QList<DeepAttributionUpdate>>();
        QCOMPARE(updates.size(), 1);
        QCOMPARE(updates.front().process.pid, qint32(4242));
        QCOMPARE(updates.front().reason, AttributionReason::Resolved);
        QVERIFY(resolver.calls() >= 3);

        // Queue drains after success — no perpetual churn.
        QTRY_COMPARE_WITH_TIMEOUT(worker.pendingCount(), 0, 1000);
    }

    void agesOutUnresolvableFlowWithoutEmitting()
    {
        LateResolver resolver(/*attributeOnCall=*/9999); // never attributes
        ResolverDeepWorker worker;
        worker.setResolver(&resolver);
        ResolverTuning t = fastTuning();
        t.deepMaxAttempts = 4; // give up quickly
        worker.setTuning(t);

        QSignalSpy spy(&worker, &DeepAttributionWorker::refined);
        worker.enqueue({mkRequest()});

        // It should retry up to the attempt cap then drop the request — never
        // emitting a refinement for a flow it can't attribute.
        QTRY_COMPARE_WITH_TIMEOUT(worker.pendingCount(), 0, 2000);
        QCOMPARE(spy.count(), 0);
    }

    void inactiveWorkerIgnoresEnqueue()
    {
        LateResolver resolver(1);
        ResolverDeepWorker worker;
        worker.setResolver(&resolver);
        worker.setTuning(fastTuning());
        worker.setActive(false);

        QSignalSpy spy(&worker, &DeepAttributionWorker::refined);
        worker.enqueue({mkRequest()});
        QCOMPARE(worker.pendingCount(), 0);

        // Give any stray timer a chance; nothing should fire.
        QTest::qWait(50);
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TestDeepWorker)
#include "test_deep_worker.moc"
