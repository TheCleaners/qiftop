// Tests for qiftop::agent::attributeFlows — the helper that enriches a
// snapshot of Connection rows with process + container metadata from
// the wired ProcessResolver. Drives the resolver via a fake so the
// test stays a pure unit test (no /proc, no sock_diag, no agent
// process).
//
// Pins the per-tick PID memoisation: the helper may resolve a PID for
// each unique 4-tuple, but must call into the resolver at most once per
// unique PID for process enrichment and container resolution.

#include <QTest>

#include "agent/Attribution.h"
#include "backend/Connection.h"
#include "backend/ProcessResolver.h"

using qiftop::backend::ContainerInfo;
using qiftop::backend::ProcessInfo;
using qiftop::backend::ProcessResolver;

namespace {

// Configurable fake. Records call counts so the memoisation assertion
// is testable. resolvePid returns the PID mapped via setProcessFor();
// enrichPid returns the ProcessInfo for that PID; resolveContainerForPid
// returns whatever container was mapped via setContainerFor(pid).
class FakeResolver : public ProcessResolver {
public:
    bool initialize() override { return true; }
    QStringList capabilities() const override { return m_caps; }

    qint32 resolvePid(const Connection &flow) override
    {
        ++pidCalls;
        const auto key = QString::number(flow.local.port);
        if (auto it = m_processByLocalPort.constFind(key);
            it != m_processByLocalPort.constEnd())
            return it.value().pid;
        return 0;
    }

    std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        ++enrichCalls[pid];
        if (enrichGenerator) return enrichGenerator(pid, enrichCalls[pid]);
        if (auto it = m_processByPid.constFind(pid);
            it != m_processByPid.constEnd())
            return it.value();
        return std::nullopt;
    }

    std::optional<ContainerInfo> resolveContainerForPid(qint32 pid) override
    {
        ++containerCalls[pid];
        if (containerGenerator) return containerGenerator(pid, containerCalls[pid]);
        if (auto it = m_containerByPid.constFind(pid);
            it != m_containerByPid.constEnd())
            return it.value();
        return std::nullopt;
    }

    QList<ContainerInfo> resolveContainerChainForPid(qint32 pid) override
    {
        ++chainCalls[pid];
        if (auto it = m_chainByPid.constFind(pid);
            it != m_chainByPid.constEnd())
            return it.value();
        // Mirror the base-class default: wrap single into list.
        if (auto ci = resolveContainerForPid(pid)) return { *ci };
        return {};
    }

    void setCapabilities(QStringList c) { m_caps = std::move(c); }
    void setProcessForLocalPort(quint16 p, ProcessInfo info)
    {
        m_processByPid.insert(info.pid, info);
        m_processByLocalPort.insert(QString::number(p), info);
    }
    void setContainerForPid(qint32 pid, ContainerInfo c)
    { m_containerByPid.insert(pid, c); }
    void setChainForPid(qint32 pid, QList<ContainerInfo> chain)
    { m_chainByPid.insert(pid, std::move(chain)); }

    int pidCalls = 0;
    QHash<qint32, int> enrichCalls;
    QHash<qint32, int> containerCalls;
    QHash<qint32, int> chainCalls;

    // Optional per-call generators (PID-reuse tests): when set, they
    // take precedence over the static maps and receive the per-PID
    // call number so each "incarnation" can return different data.
    std::function<std::optional<ProcessInfo>(qint32 pid, int callNo)>
        enrichGenerator;
    std::function<std::optional<ContainerInfo>(qint32 pid, int callNo)>
        containerGenerator;

private:
    QStringList                 m_caps;
    QHash<QString, ProcessInfo> m_processByLocalPort;
    QHash<qint32, ProcessInfo>  m_processByPid;
    QHash<qint32, ContainerInfo>m_containerByPid;
    QHash<qint32, QList<ContainerInfo>> m_chainByPid;
};

Connection makeFlow(quint16 localPort)
{
    Connection c;
    c.proto = L4Proto::Tcp;
    c.local.address  = QHostAddress(QStringLiteral("10.0.0.5"));
    c.local.port     = localPort;
    c.remote.address = QHostAddress(QStringLiteral("203.0.113.7"));
    c.remote.port    = 443;
    return c;
}

} // namespace

class TestAttribution : public QObject {
    Q_OBJECT
private slots:
    void nullResolverIsNoop()
    {
        QList<Connection> flows = { makeFlow(8080), makeFlow(8081) };
        qiftop::agent::attributeFlows(flows, nullptr);
        for (const auto &c : flows) {
            QCOMPARE(c.process.pid, qint32(0));
            QVERIFY(c.container.runtime.isEmpty());
            QVERIFY(c.containerChain.isEmpty());
        }
    }

    void resolverWithNoMatchLeavesDefaults()
    {
        // resolver advertises everything but knows nothing about our flows
        FakeResolver r;
        r.setCapabilities({QStringLiteral("process-attribution"),
                           QStringLiteral("container-attribution")});
        QList<Connection> flows = { makeFlow(8080) };
        qiftop::agent::attributeFlows(flows, &r);
        QCOMPARE(flows[0].process.pid, qint32(0));
        QVERIFY(flows[0].container.runtime.isEmpty());
    }

    void processOnlyAttribution()
    {
        FakeResolver r;
        r.setCapabilities({QStringLiteral("process-attribution")});
        ProcessInfo p; p.pid = 1234; p.uid = 33; p.comm = QStringLiteral("nginx");
        r.setProcessForLocalPort(8080, p);

        QList<Connection> flows = { makeFlow(8080) };
        qiftop::agent::attributeFlows(flows, &r);
        QCOMPARE(flows[0].process.pid, qint32(1234));
        QCOMPARE(flows[0].process.uid, quint32(33));
        QCOMPARE(flows[0].process.comm, QStringLiteral("nginx"));
        // No container known — leave empty.
        QVERIFY(flows[0].container.runtime.isEmpty());
        QVERIFY(flows[0].containerChain.isEmpty());
    }

    void processAndContainerAttribution()
    {
        FakeResolver r;
        ProcessInfo p; p.pid = 1234; p.uid = 33; p.comm = QStringLiteral("nginx");
        r.setProcessForLocalPort(8080, p);
        r.setContainerForPid(1234,
            ContainerInfo{QStringLiteral("docker"),
                          QStringLiteral("af85275074f5"),
                          QStringLiteral("web")});

        QList<Connection> flows = { makeFlow(8080) };
        qiftop::agent::attributeFlows(flows, &r);
        QCOMPARE(flows[0].container.runtime, QStringLiteral("docker"));
        QCOMPARE(flows[0].container.id,      QStringLiteral("af85275074f5"));
        QCOMPARE(flows[0].container.name,    QStringLiteral("web"));
        // Chain stays empty when wantContainerChain=false (the default),
        // even though the resolver could synthesise a single-entry one —
        // the wire must match the advertised capability.
        QVERIFY(flows[0].containerChain.isEmpty());
    }

    void containerChainOptIn()
    {
        FakeResolver r;
        ProcessInfo p; p.pid = 1234; p.uid = 33; p.comm = QStringLiteral("nginx");
        r.setProcessForLocalPort(8080, p);
        r.setChainForPid(1234, {
            ContainerInfo{QStringLiteral("kubernetes"),
                          QStringLiteral("pod-uid"), {}},
            ContainerInfo{QStringLiteral("containerd"),
                          QStringLiteral("cid12345678"),
                          QStringLiteral("app")},
        });
        // Need a leaf container too (the helper does both).
        r.setContainerForPid(1234,
            ContainerInfo{QStringLiteral("containerd"),
                          QStringLiteral("cid12345678"),
                          QStringLiteral("app")});

        QList<Connection> flows = { makeFlow(8080) };
        qiftop::agent::attributeFlows(flows, &r,
            qiftop::agent::AttributionOptions{ /*wantContainerChain=*/true });

        QCOMPARE(flows[0].containerChain.size(), 2);
        QCOMPARE(flows[0].containerChain[0].runtime,
                 QStringLiteral("kubernetes"));
        QCOMPARE(flows[0].containerChain[1].id,
                 QStringLiteral("cid12345678"));
    }

    void memoisesByPid()
    {
        // 50 flows from the same PID must yield 1 enrichPid and
        // 1 resolveContainerForPid call, not 50. PID resolution is
        // per-flow because each flow's 5-tuple is genuinely unique.
        FakeResolver r;
        ProcessInfo p; p.pid = 999; p.uid = 0; p.comm = QStringLiteral("redis");
        for (quint16 port = 9000; port < 9050; ++port) {
            r.setProcessForLocalPort(port, p);
        }
        r.setContainerForPid(999,
            ContainerInfo{QStringLiteral("docker"),
                          QStringLiteral("abc123def456"),
                          QStringLiteral("cache")});

        QList<Connection> flows;
        for (quint16 port = 9000; port < 9050; ++port)
            flows << makeFlow(port);

        qiftop::agent::attributeFlows(flows, &r);

        QCOMPARE(r.pidCalls, 50);
        QCOMPARE(r.enrichCalls.value(999), 1);
        QCOMPARE(r.containerCalls.value(999), 1);
        // Every flow got the container.
        for (const auto &c : flows) {
            QCOMPARE(c.container.id, QStringLiteral("abc123def456"));
        }
    }

    void attributionMemoisesProcessLookupByPid()
    {
        // 50 flows across 5 PIDs: resolvePid remains per-flow, while
        // enrichPid (the /proc-heavy operation in real resolvers) is
        // called once per unique PID.
        FakeResolver r;
        QList<Connection> flows;
        for (qint32 pid = 1000; pid < 1005; ++pid) {
            ProcessInfo p;
            p.pid = pid;
            p.uid = quint32(pid);
            p.comm = QStringLiteral("svc%1").arg(pid);
            for (quint16 offset = 0; offset < 10; ++offset) {
                const quint16 port = quint16(10000 + (pid - 1000) * 10 + offset);
                r.setProcessForLocalPort(port, p);
                flows << makeFlow(port);
            }
        }

        qiftop::agent::attributeFlows(flows, &r);

        QCOMPARE(r.pidCalls, 50);
        for (qint32 pid = 1000; pid < 1005; ++pid) {
            QCOMPARE(r.enrichCalls.value(pid), 1);
        }
        for (const auto &c : flows) {
            QVERIFY(c.process.valid());
            QVERIFY(c.process.comm.startsWith(QStringLiteral("svc")));
        }
    }

    void chainAlsoMemoisesByPid()
    {
        FakeResolver r;
        ProcessInfo p; p.pid = 42; p.uid = 0; p.comm = QStringLiteral("svc");
        for (quint16 port = 7000; port < 7010; ++port)
            r.setProcessForLocalPort(port, p);
        r.setContainerForPid(42,
            ContainerInfo{QStringLiteral("podman"),
                          QStringLiteral("p12345"),
                          QStringLiteral("svc")});
        r.setChainForPid(42, {
            ContainerInfo{QStringLiteral("podman"),
                          QStringLiteral("p12345"),
                          QStringLiteral("svc")},
        });

        QList<Connection> flows;
        for (quint16 port = 7000; port < 7010; ++port)
            flows << makeFlow(port);

        qiftop::agent::attributeFlows(flows, &r,
            qiftop::agent::AttributionOptions{ /*wantContainerChain=*/true });

        QCOMPARE(r.containerCalls.value(42), 1);
        QCOMPARE(r.chainCalls.value(42), 1);
    }

    void flowWithoutPidStaysBare()
    {
        // A flow the resolver can't attribute (e.g. a forwarded flow)
        // must NOT trigger a container lookup against PID 0.
        FakeResolver r;
        QList<Connection> flows = { makeFlow(8080) };
        qiftop::agent::attributeFlows(flows, &r);
        QCOMPARE(r.pidCalls, 1);
        QVERIFY(r.enrichCalls.isEmpty());
        QVERIFY(r.containerCalls.isEmpty());
    }

    void pidReuseWithinPassGetsFreshAttribution()
    {
        // H2 / AGENTS.md §8a rule 2: two flows resolve the SAME pid,
        // but the kernel recycled it between them (the injected
        // starttime probe reports a different starttime for each
        // flow). The (pid, starttime) memo must trigger a SECOND
        // resolver lookup for the new incarnation instead of serving
        // the previous owner's process/container attribution.
        FakeResolver r;
        ProcessInfo p; p.pid = 777; p.uid = 1; p.comm = QStringLiteral("seed");
        r.setProcessForLocalPort(8080, p);
        r.setProcessForLocalPort(8081, p);   // both flows resolve pid 777
        r.enrichGenerator = [](qint32 pid, int callNo) {
            ProcessInfo i;
            i.pid  = pid;
            i.uid  = 1;
            i.comm = QStringLiteral("incarnation%1").arg(callNo);
            return std::optional<ProcessInfo>(i);
        };
        r.containerGenerator = [](qint32, int callNo) {
            return std::optional<ContainerInfo>(
                ContainerInfo{QStringLiteral("docker"),
                              QStringLiteral("cid-%1").arg(callNo), {}});
        };

        qiftop::agent::AttributionOptions opts;
        quint64 nextStart = 100;
        opts.startTimeForPid = [&nextStart](qint32) { return nextStart++; };

        QList<Connection> flows = { makeFlow(8080), makeFlow(8081) };
        qiftop::agent::attributeFlows(flows, &r, opts);

        // No cross-contamination: each incarnation got its own lookup.
        QCOMPARE(r.enrichCalls.value(777), 2);
        QCOMPARE(r.containerCalls.value(777), 2);
        QCOMPARE(flows[0].process.comm, QStringLiteral("incarnation1"));
        QCOMPARE(flows[1].process.comm, QStringLiteral("incarnation2"));
        QCOMPARE(flows[0].container.id, QStringLiteral("cid-1"));
        QCOMPARE(flows[1].container.id, QStringLiteral("cid-2"));
    }

    void samePidSameStartTimeStillMemoised()
    {
        // The (pid, starttime) key must NOT cost the memo its benefit:
        // 50 flows from one stable (pid, starttime) = 1 enrich + 1
        // container lookup, exactly as before.
        FakeResolver r;
        ProcessInfo p; p.pid = 777; p.uid = 1; p.comm = QStringLiteral("redis");
        for (quint16 port = 9100; port < 9150; ++port)
            r.setProcessForLocalPort(port, p);
        r.setContainerForPid(777,
            ContainerInfo{QStringLiteral("docker"),
                          QStringLiteral("abc123def456"), {}});

        qiftop::agent::AttributionOptions opts;
        opts.startTimeForPid = [](qint32) { return quint64(100); };

        QList<Connection> flows;
        for (quint16 port = 9100; port < 9150; ++port)
            flows << makeFlow(port);
        qiftop::agent::attributeFlows(flows, &r, opts);

        QCOMPARE(r.pidCalls, 50);
        QCOMPARE(r.enrichCalls.value(777), 1);
        QCOMPARE(r.containerCalls.value(777), 1);
        for (const auto &c : flows)
            QCOMPARE(c.container.id, QStringLiteral("abc123def456"));
    }
};

QTEST_GUILESS_MAIN(TestAttribution)
#include "test_attribution.moc"
