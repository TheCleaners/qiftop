// Pins the TRANSPORT-NEUTRAL backend capability contract (AGENTS.md §4):
// each monitor reports the tokens ITS data path actually delivers, and the
// client gates UI on the union — agent proxy OR in-process backend. This test
// nails down per-backend token sets + the merge helper so a future refactor
// can't silently regress a backend back to "advertises nothing" (which used
// to hide the attribution columns whenever we ran in-process).

#include <QStringList>
#include <QtTest/QtTest>

#include <memory>
#include <optional>
#include <utility>

#include "backend/MonitorCapabilities.h"
#include "backend/ProcessResolver.h"
#include "backend/dbus/DBusConnectionMonitor.h"
#include "backend/dbus/DBusNetworkMonitor.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ConntrackMonitor.h"
#include "backend/linux/NetlinkMonitor.h"
#endif

#ifdef BACKEND_BSD
#include "backend/bsd/BsdConnectionMonitor.h"
#include "backend/bsd/BsdNetworkMonitor.h"
#endif

using qiftop::backend::mergeCapabilities;

namespace {
// Order-independent comparison: clients branch on token PRESENCE, so the
// contract is "this exact SET", not "this exact list".
QSet<QString> asSet(const QStringList &l) { return {l.cbegin(), l.cend()}; }

// Minimal resolver whose only job is to report a chosen capability list, so
// we can inject it into ConntrackMonitor and pin the resolver-caps →
// *-attribution-wire mapping without needing root or a live sock_diag socket.
class FakeResolver : public qiftop::backend::ProcessResolver {
public:
    explicit FakeResolver(QStringList caps) : m_caps(std::move(caps)) {}
    bool initialize() override { return true; }
    QStringList capabilities() const override { return m_caps; }
    qint32 resolvePid(const Connection &) override { return 0; }
    std::optional<qiftop::backend::ProcessInfo> enrichPid(qint32) override
    {
        return std::nullopt;
    }
    std::optional<qiftop::backend::ContainerInfo>
        resolveContainerForPid(qint32) override
    {
        return std::nullopt;
    }

private:
    QStringList m_caps;
};
} // namespace

class TestBackendCapabilities : public QObject {
    Q_OBJECT

private slots:
    // ---- the pure union helper -------------------------------------------
    void mergeDedupsAndPreservesOrder()
    {
        const QStringList net{QStringLiteral("ifindex"),
                              QStringLiteral("oper-state")};
        const QStringList conn{QStringLiteral("oper-state"),   // dup
                               QStringLiteral("iana-proto")};
        const QStringList merged = mergeCapabilities(net, conn);
        // First-seen order, no duplicates.
        QCOMPARE(merged, (QStringList{QStringLiteral("ifindex"),
                                      QStringLiteral("oper-state"),
                                      QStringLiteral("iana-proto")}));
    }

    void mergeHandlesEmpties()
    {
        QVERIFY(mergeCapabilities(QStringList{}, QStringList{}).isEmpty());
        const QStringList one{QStringLiteral("ifindex")};
        QCOMPARE(mergeCapabilities(one, {}), one);
        QCOMPARE(mergeCapabilities({}, one), one);
    }

    // ---- the resolver-caps → *-attribution-wire mapping ------------------
    // Shared by the DBus agent (InterfacesService) and the in-process
    // ConntrackMonitor, so pin it once as a pure function.
    void attributionWireTokensMapping()
    {
        using qiftop::backend::attributionWireTokens;
        // Nothing in → nothing out.
        QVERIFY(attributionWireTokens({}).isEmpty());
        // process-attribution alone lights only the process wire token.
        QCOMPARE(asSet(attributionWireTokens({QStringLiteral("process-attribution")})),
                 (QSet<QString>{QStringLiteral("process-attribution-wire")}));
        // container-attribution alone lights only the container wire token.
        QCOMPARE(asSet(attributionWireTokens({QStringLiteral("container-attribution")})),
                 (QSet<QString>{QStringLiteral("container-attribution-wire")}));
        // chain-wire is a STRICT superset: container-chain without
        // container-attribution must NOT light it.
        QVERIFY(!asSet(attributionWireTokens({QStringLiteral("container-chain")}))
                     .contains(QStringLiteral("container-chain-wire")));
        // The full resolver lights all three.
        QCOMPARE(asSet(attributionWireTokens({QStringLiteral("process-attribution"),
                                              QStringLiteral("container-attribution"),
                                              QStringLiteral("container-chain")})),
                 (QSet<QString>{QStringLiteral("process-attribution-wire"),
                                QStringLiteral("container-attribution-wire"),
                                QStringLiteral("container-chain-wire")}));
        // Unknown tokens are ignored.
        QVERIFY(attributionWireTokens({QStringLiteral("netns-scan")}).isEmpty());
    }

    // ---- DBus proxies: agent's merged list rides on the network proxy ----
    void dbusNetworkCarriesAgentCaps()
    {
        qiftop::backend::dbus_client::DBusNetworkMonitor net;
        // Default: nothing until main seeds it from probeAgent().
        QVERIFY(net.capabilities().isEmpty());

        const QStringList agentCaps{
            QStringLiteral("process-attribution-wire"),
            QStringLiteral("ifindex"),
            QStringLiteral("tcp-state"),
        };
        net.setAgentCapabilities(agentCaps);
        QCOMPARE(net.capabilities(), agentCaps);
    }

    void dbusConnectionReportsEmpty()
    {
        // The connection proxy deliberately reports empty — the agent exposes
        // ONE merged list on the Interfaces service, so only the network proxy
        // carries it; the client's union recombines.
        qiftop::backend::dbus_client::DBusConnectionMonitor conn;
        QVERIFY(conn.capabilities().isEmpty());
    }

    void dbusUnionEqualsAgentList()
    {
        qiftop::backend::dbus_client::DBusNetworkMonitor    net;
        qiftop::backend::dbus_client::DBusConnectionMonitor conn;
        const QStringList agentCaps{
            QStringLiteral("ifindex"),
            QStringLiteral("oper-state"),
            QStringLiteral("process-attribution-wire"),
        };
        net.setAgentCapabilities(agentCaps);
        QCOMPARE(asSet(mergeCapabilities(&net, &conn)), asSet(agentCaps));
    }

#ifdef BACKEND_LINUX
    // ---- in-process Linux backends: only what they genuinely deliver -----
    void netlinkAdvertisesInterfaceTokens()
    {
        NetlinkMonitor net;
        QCOMPARE(asSet(net.capabilities()),
                 (QSet<QString>{QStringLiteral("ifindex"),
                                QStringLiteral("oper-state"),
                                QStringLiteral("link-errors")}));
    }

    void conntrackAdvertisesOnlyStructuralTokens()
    {
        // Inject an inert resolver (empty caps) so the assertion is
        // deterministic regardless of the CI host's privileges — the default
        // ctor now wires the real platform resolver, whose probed caps depend
        // on whether sock_diag/cgroup access is available.
        ConntrackMonitor conn(std::make_unique<FakeResolver>(QStringList{}));
        const QSet<QString> caps = asSet(conn.capabilities());
        // Structural caps the dump genuinely fills.
        QCOMPARE(caps, (QSet<QString>{QStringLiteral("iana-proto"),
                                      QStringLiteral("tcp-state")}));
        // No (useful) resolver wired ⇒ MUST NOT claim attribution.
        // Direction/reason are inferred client-side, not produced here.
        QVERIFY(!caps.contains(QStringLiteral("process-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-chain-wire")));
        QVERIFY(!caps.contains(QStringLiteral("direction-on-wire")));
        QVERIFY(!caps.contains(QStringLiteral("attribution-reason")));
    }

    void conntrackResolverLightsAttributionWire()
    {
        // A resolver advertising the full attribution stack must light all
        // three *-attribution-wire tokens (the in-process self-elevated path
        // now attributes exactly like the agent), on top of the structural
        // tokens — but still not direction/reason (client-inferred).
        ConntrackMonitor conn(std::make_unique<FakeResolver>(QStringList{
            QStringLiteral("process-attribution"),
            QStringLiteral("container-attribution"),
            QStringLiteral("container-chain"),
            QStringLiteral("netns-scan")}));
        const QSet<QString> caps = asSet(conn.capabilities());
        QVERIFY(caps.contains(QStringLiteral("iana-proto")));
        QVERIFY(caps.contains(QStringLiteral("tcp-state")));
        QVERIFY(caps.contains(QStringLiteral("process-attribution-wire")));
        QVERIFY(caps.contains(QStringLiteral("container-attribution-wire")));
        QVERIFY(caps.contains(QStringLiteral("container-chain-wire")));
        // Still not produced in-process.
        QVERIFY(!caps.contains(QStringLiteral("direction-on-wire")));
        QVERIFY(!caps.contains(QStringLiteral("attribution-reason")));
    }

    void conntrackProcessOnlyResolver()
    {
        // A thinner resolver (process attribution only, e.g. unprivileged
        // with no cgroup access) lights only the process wire token.
        ConntrackMonitor conn(std::make_unique<FakeResolver>(QStringList{
            QStringLiteral("process-attribution")}));
        const QSet<QString> caps = asSet(conn.capabilities());
        QVERIFY(caps.contains(QStringLiteral("process-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-chain-wire")));
    }

    void inProcessLinuxUnionLightsAttributionViaResolver()
    {
        // With an attribution-capable resolver wired into the in-process
        // flow backend, the in-process Linux union now lights the
        // attribution wire tokens — so the GUI/TUI Process/Container columns
        // become available on the self-elevated path, at parity with the
        // agent. (Without a useful resolver the union stays attribution-free,
        // covered by conntrackAdvertisesOnlyStructuralTokens.)
        NetlinkMonitor   net;
        ConntrackMonitor conn(std::make_unique<FakeResolver>(QStringList{
            QStringLiteral("process-attribution"),
            QStringLiteral("container-attribution")}));
        const QSet<QString> u = asSet(mergeCapabilities(&net, &conn));
        QVERIFY(u.contains(QStringLiteral("ifindex")));
        QVERIFY(u.contains(QStringLiteral("iana-proto")));
        QVERIFY(u.contains(QStringLiteral("process-attribution-wire")));
        QVERIFY(u.contains(QStringLiteral("container-attribution-wire")));
    }
#endif

#ifdef BACKEND_BSD
    // ---- in-process BSD backends: they DO attribute, so columns light up --
    void bsdNetworkAdvertisesInterfaceTokens()
    {
        qiftop::backend::bsd::BsdNetworkMonitor net;
        QCOMPARE(asSet(net.capabilities()),
                 (QSet<QString>{QStringLiteral("ifindex"),
                                QStringLiteral("oper-state"),
                                QStringLiteral("link-errors")}));
    }

    void bsdConnectionAdvertisesAttribution()
    {
        qiftop::backend::bsd::BsdConnectionMonitor conn;
        const QSet<QString> caps = asSet(conn.capabilities());
        QVERIFY(caps.contains(QStringLiteral("iana-proto")));
        QVERIFY(caps.contains(QStringLiteral("direction-on-wire")));
        // BsdSocketResolver attributes a PID on every BSD.
        QVERIFY(caps.contains(QStringLiteral("process-attribution-wire")));
        // FreeBSD additionally tags jailed flows; the other BSDs don't.
#  ifdef __FreeBSD__
        QVERIFY(caps.contains(QStringLiteral("container-attribution-wire")));
#  else
        QVERIFY(!caps.contains(QStringLiteral("container-attribution-wire")));
#  endif
        // pcap has no conntrack state machine, and there's no nesting model.
        QVERIFY(!caps.contains(QStringLiteral("tcp-state")));
        QVERIFY(!caps.contains(QStringLiteral("container-chain-wire")));
    }
#endif
};

QTEST_MAIN(TestBackendCapabilities)
#include "test_backend_capabilities.moc"
