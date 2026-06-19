// Unit tests for BpfBirthResolver — the birth-cache process resolver that sits
// first in the chain. No kernel / BPF: births are injected via onBirth(), and
// PID-reuse validation uses an injected starttime probe + clock.

#include <QTest>

#include "backend/BpfBirthResolver.h"
#include "backend/CompositeResolver.h"

using namespace qiftop::backend;

namespace {

Connection mkFlow(quint16 localPort, const char *remote = "1.1.1.1",
                  quint16 remotePort = 443, L4Proto p = L4Proto::Tcp)
{
    Connection c;
    c.proto          = p;
    c.local.address  = QHostAddress(QStringLiteral("10.0.0.1"));
    c.local.port     = localPort;
    c.remote.address = QHostAddress(QString::fromLatin1(remote));
    c.remote.port    = remotePort;
    return c;
}

BirthRecord mkRec(qint32 pid, const char *comm, quint64 startTime)
{
    BirthRecord r;
    r.pid = pid; r.uid = 1000; r.comm = QString::fromLatin1(comm);
    r.direction = Direction::Outbound; r.startTime = startTime;
    return r;
}

// A fall-through resolver that attributes a fixed pid by local port, so we can
// verify BpfBirthResolver yields to it on a cache miss.
class StubResolver final : public ProcessResolver {
public:
    QHash<quint16, qint32> pidByLocalPort;
    int resolvePidCalls = 0;
    bool initialize() override { return true; }
    QStringList capabilities() const override { return {QStringLiteral("process-attribution")}; }
    qint32 resolvePid(const Connection &f) override
    { ++resolvePidCalls; return pidByLocalPort.value(f.local.port, 0); }
    std::optional<ProcessInfo> enrichPid(qint32 pid) override
    { ProcessInfo p; p.pid = pid; p.comm = QStringLiteral("stub"); return p; }
    std::optional<ContainerInfo> resolveContainerForPid(qint32) override { return std::nullopt; }
};

} // namespace

class TestBpfBirthResolver : public QObject {
    Q_OBJECT

private slots:
    void inertUntilLoaded()
    {
        BpfBirthResolver r;
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));
        // Not loaded → contributes nothing.
        QVERIFY(!r.initialize());
        QVERIFY(r.capabilities().isEmpty());
        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(0));

        r.setLoaded(true);
        QVERIFY(r.initialize());
        QVERIFY(r.capabilities().contains(QStringLiteral("birth-attribution")));
        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(4242));
    }

    void resolvesCachedBirthAndEnrichesFromBirth()
    {
        // startTime probe agrees → pid served; comm/uid come from birth, no /proc.
        BpfBirthResolver r([](qint32){ return quint64(99); });
        r.setLoaded(true);
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));

        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(4242));
        const auto pi = r.enrichPid(4242);
        QVERIFY(pi.has_value());
        QCOMPARE(pi->comm, QStringLiteral("curl"));
        QCOMPARE(pi->uid, quint32(1000));
    }

    void rejectsRecycledPidViaStarttime()
    {
        // Cached birth says starttime=99; the live pid now reports 12345 → the
        // kernel recycled the pid. Must NOT serve the stale attribution.
        BpfBirthResolver r([](qint32){ return quint64(12345); });
        r.setLoaded(true);
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));

        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(0));
        // And the stale entry was evicted, so a second call is also a miss.
        QCOMPARE(r.cacheSize(), 0);
    }

    void goneShortLivedPidIsServed()
    {
        // Probe returns 0 (pid gone / unreadable). This is the PRIMARY hybrid
        // case: the short-lived process has exited by the time the conntrack
        // flow resolves, so /proc/<pid> is gone — but the captured (pid, comm)
        // is the historically-correct owner. Serve it (no live process holds
        // the pid, so there is nothing to be confused with).
        BpfBirthResolver r([](qint32){ return quint64(0); });
        r.setLoaded(true);
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));
        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(4242));
    }

    void cacheMissReturnsZero()
    {
        BpfBirthResolver r([](qint32){ return quint64(99); });
        r.setLoaded(true);
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));
        // Different flow → no birth → 0 (composite will fall through).
        QCOMPARE(r.resolvePid(mkFlow(6000)), qint32(0));
    }

    void ttlExpiryViaInjectedClock()
    {
        qint64 now = 1000;
        BpfBirthResolver r([](qint32){ return quint64(99); }, [&now]{ return now; });
        r.setLoaded(true);
        r.onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "curl", 99));

        now = 1000 + BirthCache::kDefaultTtlMs - 1;
        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(4242)); // within TTL
        now = 1000 + BirthCache::kDefaultTtlMs + 1;
        QCOMPARE(r.resolvePid(mkFlow(5000)), qint32(0));     // expired
    }

    void compositeBirthFirstThenSockDiagFallback()
    {
        // The production chain shape: BpfBirthResolver first, a sock_diag-like
        // stub second. Birth wins for the flow it saw; the stub attributes the
        // one birth missed.
        auto birth = std::make_unique<BpfBirthResolver>([](qint32){ return quint64(99); });
        birth->setLoaded(true);
        birth->onBirth(birthKeyOf(mkFlow(5000)), mkRec(4242, "shortlived", 99));
        auto *birthRaw = birth.get();

        auto stub = std::make_unique<StubResolver>();
        stub->pidByLocalPort.insert(6000, 777); // only the non-birth flow
        auto *stubRaw = stub.get();

        CompositeResolver chain;
        chain.add(std::move(birth));
        chain.add(std::move(stub));

        // Flow 5000: birth attributes it; the stub is never consulted.
        QCOMPARE(chain.resolvePid(mkFlow(5000)), qint32(4242));
        QCOMPARE(stubRaw->resolvePidCalls, 0);

        // Flow 6000: birth misses → falls through to the stub.
        QCOMPARE(chain.resolvePid(mkFlow(6000)), qint32(777));
        QCOMPARE(stubRaw->resolvePidCalls, 1);

        // Capabilities are the union, including birth-attribution.
        QVERIFY(chain.capabilities().contains(QStringLiteral("birth-attribution")));
        QVERIFY(chain.capabilities().contains(QStringLiteral("process-attribution")));
        Q_UNUSED(birthRaw);
    }

    void udpBirthResolves()
    {
        BpfBirthResolver r([](qint32){ return quint64(99); });
        r.setLoaded(true);
        const auto udp = mkFlow(40000, "203.0.113.7", 53, L4Proto::Udp);
        r.onBirth(birthKeyOf(udp), mkRec(888, "resolver", 99));
        QCOMPARE(r.resolvePid(udp), qint32(888));
        // A TCP flow on the same tuple must NOT match the UDP birth.
        const auto tcp = mkFlow(40000, "203.0.113.7", 53, L4Proto::Tcp);
        QCOMPARE(r.resolvePid(tcp), qint32(0));
    }
};

QTEST_APPLESS_MAIN(TestBpfBirthResolver)
#include "test_bpf_birth_resolver.moc"
