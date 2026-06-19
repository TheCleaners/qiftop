// Unit tests for qiftop::backend::CompositeResolver.
//
// Pins TESTS-C1 from the v0.2 audit: every Linux user's flow attribution
// chain runs through CompositeResolver, and prior to this test the only
// coverage was the resolver factory's default-config path which returns
// the Null resolver. The composite's first-non-nullopt fan-out,
// de-duplicated capability union, and (critically) its OVERRIDE of the
// base-class resolveContainerChainForPid default were entirely untested.
//
// What this file does NOT test: live /proc, sock_diag, or cgroup
// parsing — see test_sockdiag_parse / test_cgroup_parse for those.

#include <QTest>

#include "backend/CompositeResolver.h"
#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

#include <memory>
#include <optional>

using qiftop::backend::CompositeResolver;
using qiftop::backend::ContainerInfo;
using qiftop::backend::ProcessInfo;
using qiftop::backend::ProcessResolver;

namespace {

// Programmable in-memory resolver. Tests stage canned answers per
// method; null-by-default lets the composite walk to the next child.
class FakeResolver : public ProcessResolver {
public:
    QStringList caps;

    // resolvePid: keyed by remote port for simplicity (the composite
    // only cares that the value is non-zero to short-circuit).
    QMap<quint16, qint32> pidByRemotePort;

    // enrichPid / resolveContainerForPid: keyed by pid.
    QMap<qint32, ProcessInfo>   processByPid;
    QMap<qint32, ContainerInfo> containerByPid;
    QMap<qint32, QList<ContainerInfo>> chainByPid;

    // Override-vs-default flag: when set, the child returns whatever
    // chainByPid says (possibly empty). When NOT set, the base-class
    // default kicks in (wrap resolveContainerForPid result).
    bool overrideChain = true;

    int callsResolvePid = 0;
    int callsEnrichPid = 0;
    int callsContainer = 0;
    int callsChain = 0;
    int callsDeepScan = 0;

    bool initialize() override { return true; }
    QStringList capabilities() const override { return caps; }

    void requestDeepScan() override { ++callsDeepScan; }

    qint32 resolvePid(const Connection &flow) override
    {
        ++callsResolvePid;
        return pidByRemotePort.value(flow.remote.port, 0);
    }
    std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        ++callsEnrichPid;
        if (auto it = processByPid.find(pid); it != processByPid.end())
            return *it;
        return std::nullopt;
    }
    std::optional<ContainerInfo> resolveContainerForPid(qint32 pid) override
    {
        ++callsContainer;
        if (auto it = containerByPid.find(pid); it != containerByPid.end())
            return *it;
        return std::nullopt;
    }
    QList<ContainerInfo> resolveContainerChainForPid(qint32 pid) override
    {
        ++callsChain;
        if (!overrideChain)
            return ProcessResolver::resolveContainerChainForPid(pid);
        return chainByPid.value(pid, {});
    }
};

Connection mkFlow(quint16 remotePort)
{
    Connection c;
    c.remote.port = remotePort;
    return c;
}

ProcessInfo mkProc(qint32 pid, const char *comm)
{
    ProcessInfo p;
    p.pid = pid;
    p.comm = QString::fromLatin1(comm);
    return p;
}

ContainerInfo mkCtr(const char *runtime, const char *id, const char *name = "")
{
    return ContainerInfo{
        QString::fromLatin1(runtime),
        QString::fromLatin1(id),
        QString::fromLatin1(name),
    };
}

} // namespace

class TestCompositeResolver : public QObject {
    Q_OBJECT
private slots:

    // An empty composite must return null/empty for every query and
    // advertise no capabilities. add(nullptr) must be a no-op.
    void emptyCompositeReturnsNothing()
    {
        CompositeResolver c;
        QVERIFY(c.capabilities().isEmpty());
        QCOMPARE(c.resolvePid(mkFlow(80)), 0);
        QVERIFY(!c.enrichPid(42).has_value());
        QVERIFY(!c.resolveContainerForPid(42).has_value());
        QVERIFY(c.resolveContainerChainForPid(42).isEmpty());

        c.add(nullptr);  // must not crash
        QVERIFY(c.capabilities().isEmpty());
    }

    // Capability tokens from each child are unioned without duplicates,
    // preserving first-seen order. A capability listed by multiple
    // children must appear exactly once.
    void capabilitiesAreUnionedAndDedupedInFirstSeenOrder()
    {
        auto a = std::make_unique<FakeResolver>();
        a->caps = QStringList{
            QStringLiteral("process-attribution"),
            QStringLiteral("container-attribution"),
        };
        auto b = std::make_unique<FakeResolver>();
        b->caps = QStringList{
            QStringLiteral("container-attribution"),  // duplicate
            QStringLiteral("container-chain"),
            QStringLiteral("netns-scan"),
        };

        CompositeResolver c;
        c.add(std::move(a));
        c.add(std::move(b));

        QCOMPARE(c.capabilities(), (QStringList{
                                       QStringLiteral("process-attribution"),
                                       QStringLiteral("container-attribution"),
                                       QStringLiteral("container-chain"),
                                       QStringLiteral("netns-scan"),
                                   }));
    }

    // resolvePid: composite must return the first non-zero answer and
    // STOP walking — subsequent children are not consulted for the same
    // flow. Pins the "first-non-nullopt-wins" contract.
    void resolvePidShortCircuitsOnFirstNonZero()
    {
        auto a = std::make_unique<FakeResolver>();
        auto b = std::make_unique<FakeResolver>();
        auto c = std::make_unique<FakeResolver>();
        a->pidByRemotePort.insert(80, 0);    // miss
        b->pidByRemotePort.insert(80, 1234); // hit
        c->pidByRemotePort.insert(80, 5678); // shadowed

        auto *aRaw = a.get(), *bRaw = b.get(), *cRaw = c.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));
        comp.add(std::move(c));

        QCOMPARE(comp.resolvePid(mkFlow(80)), 1234);
        QCOMPARE(aRaw->callsResolvePid, 1);
        QCOMPARE(bRaw->callsResolvePid, 1);
        QCOMPARE(cRaw->callsResolvePid, 0);  // never reached
    }

    // enrichPid: same first-non-nullopt semantics on the PID side.
    // Validates that an empty optional from child #1 falls through to
    // child #2 instead of being treated as "answered with nothing".
    void enrichPidFallsThroughOnNullopt()
    {
        auto a = std::make_unique<FakeResolver>();
        auto b = std::make_unique<FakeResolver>();
        // a knows nothing about pid 99; b enriches it.
        b->processByPid.insert(99, mkProc(99, "nginx"));
        auto *aRaw = a.get(), *bRaw = b.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));

        const auto info = comp.enrichPid(99);
        QVERIFY(info.has_value());
        QCOMPARE(info->pid, 99);
        QCOMPARE(info->comm, QStringLiteral("nginx"));
        QCOMPARE(aRaw->callsEnrichPid, 1);  // tried
        QCOMPARE(bRaw->callsEnrichPid, 1);  // hit
    }

    // resolveContainerForPid: same semantics; ensures the per-method
    // walk is independent of the others (a child can be a hit for
    // PID but a miss for container, and the composite still walks).
    void resolveContainerFallsThroughOnNullopt()
    {
        auto a = std::make_unique<FakeResolver>();
        auto b = std::make_unique<FakeResolver>();
        b->containerByPid.insert(42, mkCtr("docker", "abc123", "happy_einstein"));
        auto *aRaw = a.get(), *bRaw = b.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));

        const auto ci = comp.resolveContainerForPid(42);
        QVERIFY(ci.has_value());
        QCOMPARE(ci->runtime, QStringLiteral("docker"));
        QCOMPARE(ci->id,      QStringLiteral("abc123"));
        QCOMPARE(aRaw->callsContainer, 1);
        QCOMPARE(bRaw->callsContainer, 1);
    }

    // Chain semantics — the most important contract for v0.2 because
    // CompositeResolver DELIBERATELY overrides the base-class default
    // (which would silently wrap resolveContainerForPid's single answer
    // into a one-element chain) to let chain-capable children provide
    // the real OUTER→INNER ancestry. Pins:
    //   - first non-EMPTY chain wins,
    //   - an empty chain from child #1 must NOT short-circuit (it's
    //     "I don't know", not "no chain"),
    //   - a chain-capable child must NOT trigger the base-class
    //     wrap fallback even if a later child has a single
    //     resolveContainerForPid answer.
    void chainPrefersFirstNonEmptyAndBypassesBaseClassWrap()
    {
        auto a = std::make_unique<FakeResolver>();
        // a has only PID-level knowledge — its chain returns empty.
        a->processByPid.insert(7, mkProc(7, "irrelevant"));

        auto b = std::make_unique<FakeResolver>();
        b->chainByPid.insert(7, QList<ContainerInfo>{
                                    mkCtr("kubernetes", "pod-uid", "podname"),
                                    mkCtr("containerd", "cid12345"),
                                });

        // c also has a chain but is shadowed by b.
        auto c = std::make_unique<FakeResolver>();
        c->chainByPid.insert(7, QList<ContainerInfo>{
                                    mkCtr("docker", "shadowed"),
                                });

        auto *aRaw = a.get(), *bRaw = b.get(), *cRaw = c.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));
        comp.add(std::move(c));

        const auto chain = comp.resolveContainerChainForPid(7);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain[0].runtime, QStringLiteral("kubernetes"));
        QCOMPARE(chain[1].runtime, QStringLiteral("containerd"));

        QCOMPARE(aRaw->callsChain, 1);  // tried, returned empty
        QCOMPARE(bRaw->callsChain, 1);  // hit
        QCOMPARE(cRaw->callsChain, 0);  // never reached
    }

    // The single most important regression risk: if a child's
    // resolveContainerChainForPid is the base-class default (wraps
    // resolveContainerForPid), composite must NOT silently substitute
    // it for a missing override on another child. Each child's chain
    // method is sovereign; the composite never calls
    // resolveContainerForPid as a chain fallback itself.
    void chainNeverFallsBackToContainerLookupAtCompositeLevel()
    {
        // a is a "chain-naive" child (overrideChain=false) — its
        // chain method delegates to ProcessResolver's base default,
        // which calls resolveContainerForPid. We've also seeded
        // containerByPid so the base default WOULD synthesize a
        // one-element chain. Composite must respect that synthesis
        // (it's the child's own answer) — but it must NOT take a's
        // empty container result and ITSELF wrap it for some other
        // child.
        auto a = std::make_unique<FakeResolver>();
        a->overrideChain = false;
        // a has no container; its base-default chain will be empty.

        auto b = std::make_unique<FakeResolver>();
        // b has only a container answer (not a chain override). Its
        // base-default chain wraps it into a one-element list.
        b->overrideChain = false;
        b->containerByPid.insert(99, mkCtr("podman", "rootful"));

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));

        const auto chain = comp.resolveContainerChainForPid(99);
        // a returned {}; b's base default returned [{podman, rootful}].
        // Composite picks the first non-empty — b's wrap.
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0].runtime, QStringLiteral("podman"));
        QCOMPARE(chain[0].id,      QStringLiteral("rootful"));
    }

    // initialize() must call EVERY child's initialize (not short-
    // circuit) so each child gets a chance to open its sockets even
    // if an earlier one returned false. Returns true if ANY child
    // became ready, matching the documented contract.
    void initializeProbesEveryChild()
    {
        struct FlagResolver : FakeResolver {
            int initCalls = 0;
            bool initResult = false;
            bool initialize() override { ++initCalls; return initResult; }
        };

        auto a = std::make_unique<FlagResolver>();
        auto b = std::make_unique<FlagResolver>();
        auto c = std::make_unique<FlagResolver>();
        a->initResult = false;
        b->initResult = true;   // makes overall return true
        c->initResult = false;
        auto *aRaw = a.get(), *bRaw = b.get(), *cRaw = c.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));
        comp.add(std::move(c));

        QVERIFY(comp.initialize());
        QCOMPARE(aRaw->initCalls, 1);
        QCOMPARE(bRaw->initCalls, 1);
        QCOMPARE(cRaw->initCalls, 1);
    }

    void requestDeepScanFansOutToEveryChild()
    {
        auto a = std::make_unique<FakeResolver>();
        auto b = std::make_unique<FakeResolver>();
        auto *aRaw = a.get(), *bRaw = b.get();

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));

        comp.requestDeepScan();
        comp.requestDeepScan();
        QCOMPARE(aRaw->callsDeepScan, 2);
        QCOMPARE(bRaw->callsDeepScan, 2);
    }

    // resolveFlow() is inherited from the base class (resolvePid +
    // enrichPid composition). The composite doesn't override it; this
    // test pins that the inherited path still works correctly across
    // the composite's fan-out — i.e. resolvePid can hit child A while
    // enrichPid hits child B for the same flow.
    void resolveFlowComposesAcrossDifferentChildren()
    {
        auto a = std::make_unique<FakeResolver>();
        a->pidByRemotePort.insert(443, 555);
        // a knows the pid but not the enrich info.

        auto b = std::make_unique<FakeResolver>();
        b->processByPid.insert(555, mkProc(555, "curl"));

        CompositeResolver comp;
        comp.add(std::move(a));
        comp.add(std::move(b));

        const auto info = comp.resolveFlow(mkFlow(443));
        QVERIFY(info.has_value());
        QCOMPARE(info->pid, 555);
        QCOMPARE(info->comm, QStringLiteral("curl"));
    }
};

QTEST_MAIN(TestCompositeResolver)
#include "test_composite_resolver.moc"
