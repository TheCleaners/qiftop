// Pins the TRANSPORT-NEUTRAL backend capability contract (AGENTS.md §4):
// each monitor reports the tokens ITS data path actually delivers, and the
// client gates UI on the union — agent proxy OR in-process backend. This test
// nails down per-backend token sets + the merge helper so a future refactor
// can't silently regress a backend back to "advertises nothing" (which used
// to hide the attribution columns whenever we ran in-process).

#include <QStringList>
#include <QtTest/QtTest>

#include "backend/MonitorCapabilities.h"
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
        ConntrackMonitor conn;
        const QSet<QString> caps = asSet(conn.capabilities());
        // Structural caps the dump genuinely fills.
        QCOMPARE(caps, (QSet<QString>{QStringLiteral("iana-proto"),
                                      QStringLiteral("tcp-state")}));
        // No resolver wired ⇒ MUST NOT claim attribution. Direction/reason
        // are inferred client-side, not produced here.
        QVERIFY(!caps.contains(QStringLiteral("process-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-attribution-wire")));
        QVERIFY(!caps.contains(QStringLiteral("container-chain-wire")));
        QVERIFY(!caps.contains(QStringLiteral("direction-on-wire")));
        QVERIFY(!caps.contains(QStringLiteral("attribution-reason")));
    }

    void inProcessLinuxUnionLightsNoAttribution()
    {
        // The whole point: the in-process Linux union has interface +
        // structural flow tokens but NO attribution — so the GUI/TUI gate
        // keeps the Process/Container columns hidden on this path.
        NetlinkMonitor   net;
        ConntrackMonitor conn;
        const QSet<QString> u = asSet(mergeCapabilities(&net, &conn));
        QVERIFY(u.contains(QStringLiteral("ifindex")));
        QVERIFY(u.contains(QStringLiteral("iana-proto")));
        QVERIFY(!u.contains(QStringLiteral("process-attribution-wire")));
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
