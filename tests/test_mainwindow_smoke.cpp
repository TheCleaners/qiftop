// MainWindow widget-level smoke tests.
//
// Built around the real production widget tree (linked against
// qiftop_ui), driven by Qt::Test events the same way Playwright /
// Selenium drive a headless browser. Runs offscreen under
// QT_QPA_PLATFORM=offscreen so it works in CI without a display
// server. Catches the class of bug that pure-model unit tests miss —
// signal propagation between source models, proxies, views, and
// delegates; capability-gated visibility; user-event → settings →
// model flow.
//
// Each scenario:
//   1. Constructs Settings backed by a per-test QSettings sandbox
//      (so other test binaries / the developer's real qiftop config
//      are not touched).
//   2. Builds Fake monitors + DNS resolver, hands them to a real
//      MainWindow exactly like main.cpp would.
//   3. Shows the window offscreen (waits for window-exposed event so
//      delegates/views are actually laid out).
//   4. Drives interactions via Fake.emitSnapshot(), Settings setters,
//      QTest::mouseClick / keyClicks / qWait.
//   5. Asserts state via model role queries, signal spies, and
//      visible widget properties.
//
// No real network, no DBus, no kernel work. Everything is
// deterministic and finishes in <1s per scenario.

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

#include "config/Settings.h"
#include "ui/ConnectionModel.h"
#include "ui/MainWindow.h"
#include "fakes/FakeMonitors.h"

using namespace qiftop::tests;

namespace {

// Tiny RAII helper that points Settings at a per-test sandbox.
//
// CAREFUL: QSettings caches its on-disk file location process-globally
// per (org, app) the first time any instance is constructed — later
// changes to XDG_CONFIG_HOME do NOT migrate the in-memory binding.
// Two consequences for these tests:
//   (a) setting XDG_CONFIG_HOME after the first QSettings instance is
//       useless (state leaks between tests through the cached binding),
//   (b) the only reliable way to get a clean slate between scenarios is
//       to clear() the QSettings between tests OR to use distinct
//       (org, app) names per test.
// We pick (a) — calling QSettings().clear() in each sandbox's dtor —
// because the qiftop Settings class hard-codes the (org, app) pair and
// we want to exercise THAT real code path.
class SettingsSandbox {
public:
    SettingsSandbox()
    {
        m_origXdg     = qEnvironmentVariable("XDG_CONFIG_HOME");
        m_origXdgSet  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_dir.path()));
        // Force every persisted key back to its default on entry —
        // covers the case where QSettings cached its location from
        // an earlier test in the same process.
        QSettings s;
        s.clear();
        s.sync();
    }
    ~SettingsSandbox()
    {
        QSettings s;
        s.clear();
        s.sync();
        if (m_origXdgSet)
            qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_origXdg));
        else
            qunsetenv("XDG_CONFIG_HOME");
    }
    QString path() const { return m_dir.path(); }
private:
    QTemporaryDir m_dir;
    QString       m_origXdg;
    bool          m_origXdgSet = false;
};

} // namespace

class TestMainWindowSmoke : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // The fakes hand QList<InterfaceStats> / QList<Connection>
        // across the same signal/slot edge the production backends
        // use. Both are registered by main.cpp at runtime; do the
        // same here so queued connections inside MainWindow's wiring
        // still work even though our fakes emit directly.
        qRegisterMetaType<InterfaceStats>();
        qRegisterMetaType<QList<InterfaceStats>>();
        qRegisterMetaType<Connection>();
        qRegisterMetaType<QList<Connection>>();
    }

    // Bootstrap: the window must construct, show offscreen, and
    // accept a snapshot from each fake without crashing. Catches the
    // simplest possible regression — "MainWindow's ctor blew up".
    void constructAndShow()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        // Feed one of each kind of snapshot so the model/view stack
        // actually populates.
        netMon.emitSnapshot({mkIface("eth0", 2, 1000, 500)});
        connMon.emitSnapshot({mkFlow("eth0",
                                     "192.168.1.10", 54321,
                                     "1.1.1.1",      443,
                                     L4Proto::Tcp, 1000, 500)});
        QTest::qWait(50);

        // The connections view must be present and rooted at a
        // model — proves the model→proxy→groupproxy→view chain wired
        // without errors at MainWindow construction time.
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY2(connView != nullptr, "could not find connView by objectName");
        QVERIFY(connView->model() != nullptr);
    }

    // Pins bug-view-redraw end-to-end. The fix landed at the proxy
    // level (commit 3365b84) and is unit-tested in test_group_proxy,
    // but that test drives the proxy with stub data. This one runs
    // the entire production wiring: real ConnectionModel, real
    // ConnectionFilterProxy, real ConnectionGroupProxy, real
    // QTreeView. If any layer in that stack reintroduces a
    // wholesale modelReset on per-flow snapshot churn, the view's
    // expansion state and the signal spy below will catch it.
    void viewModeSwitchPreservesExpansionAndDoesNotReset()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *combo    = w.findChild<QComboBox*>(QStringLiteral("connViewModeCombo"));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(combo);
        QVERIFY(connView);

        // Seed several flows across two interfaces so grouped mode
        // has structure to display.
        connMon.emitSnapshot({
            mkFlow("eth0",  "10.0.0.10", 1001, "1.1.1.1",  443, L4Proto::Tcp, 1000, 500),
            mkFlow("eth0",  "10.0.0.10", 1002, "1.0.0.1",  443, L4Proto::Tcp, 1000, 500),
            mkFlow("wlan0", "10.0.0.11", 2001, "8.8.8.8",  53,  L4Proto::Udp, 200, 100),
        });
        QTest::qWait(50);

        // Switch to ByInterface via the combo (programmatically — the
        // model index lookup is the same code path triggered by a
        // user mouse-click).
        const int byIface = combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByInterface));
        QVERIFY(byIface >= 0);
        combo->setCurrentIndex(byIface);
        QTest::qWait(50);

        // Two groups expected (eth0, wlan0). Expand eth0 so we have
        // an expansion state to break.
        auto *groupModel = connView->model();
        QCOMPARE(groupModel->rowCount(), 2);
        const QModelIndex eth0 = groupModel->index(0, 0);
        connView->expand(eth0);
        QVERIFY(connView->isExpanded(eth0));

        // Now simulate ~10 snapshot ticks of low-amplitude churn:
        // each tick adds a flow on a NEW peer port (so source rows
        // are inserted at the end) and removes one of the previous
        // flows. Pre-3365b84, every insert/remove triggered a
        // wholesale modelReset → tree collapsed.
        QSignalSpy resetSpy(groupModel, &QAbstractItemModel::modelReset);

        // Build a stable seed and mutate it each tick. The conn-
        // model diffs by Connection::key() so we have to keep at
        // least one anchor flow alive per peer to avoid spurious
        // remove/re-insert noise.
        QList<Connection> live = {
            mkFlow("eth0",  "10.0.0.10", 1001, "1.1.1.1",  443),
            mkFlow("wlan0", "10.0.0.11", 2001, "8.8.8.8",  53,  L4Proto::Udp),
        };
        for (int tick = 0; tick < 10; ++tick) {
            // append a fresh flow each tick — these all become rows
            // 2…11 in the source over the run.
            live.append(mkFlow("eth0", "10.0.0.10",
                               quint16(3000 + tick),
                               "9.9.9.9", 443));
            connMon.emitSnapshot(live);
            QTest::qWait(20);
        }

        // The reset count MUST stay at zero across this churn. Pre-
        // fix (or any regression), every snapshot would trigger one.
        QCOMPARE(resetSpy.size(), 0);

        // And the eth0 group must still be expanded — visible proof
        // the tree didn't collapse mid-soak.
        QVERIFY(connView->isExpanded(groupModel->index(0, 0)));
    }

    // Pins UIUX-C2: column-header click sorts. The unit test
    // exercises the proxy directly; this one drives a real
    // QTreeView so a regression in setSortingEnabled wiring or
    // header signal forwarding would also be caught.
    void columnHeaderClickSortsConnectionsView()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);

        // Three flows on the same iface with distinct rx so we can
        // assert order. Stay in Flat mode (the default) so the
        // assertion compares actual flow rows.
        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp,  100, 0),
            mkFlow("eth0", "10.0.0.10", 1002, "1.0.0.1", 443, L4Proto::Tcp, 5000, 0),
            mkFlow("eth0", "10.0.0.10", 1003, "8.8.8.8", 443, L4Proto::Tcp,  500, 0),
        });
        QTest::qWait(50);

        const int rxTotal = static_cast<int>(ConnectionModel::Column::RxTotal);
        auto *m = connView->model();

        // Sort descending by rxTotal — top row should be the 5000-
        // byte flow.
        connView->sortByColumn(rxTotal, Qt::DescendingOrder);
        QTest::qWait(20);

        QCOMPARE(m->rowCount(), 3);
        const auto topConn = m->index(0, 0)
            .data(ConnectionModel::ConnectionRole).value<Connection>();
        QCOMPARE(topConn.rxBytes, quint64{5000});

        // Flip to ascending — top should now be the 100-byte flow.
        connView->sortByColumn(rxTotal, Qt::AscendingOrder);
        QTest::qWait(20);
        const auto topConn2 = m->index(0, 0)
            .data(ConnectionModel::ConnectionRole).value<Connection>();
        QCOMPARE(topConn2.rxBytes, quint64{100});
    }

    // Pins the column-visibility AND-gate (audit fix-#3, b2efd06):
    // when the agent does not advertise the wire token, the
    // Process / Container columns must stay hidden EVEN IF the
    // Settings persisted value says "show". setBackendInfo is the
    // only point where the gate runs.
    void processColumnHiddenWithoutWireCapability()
    {
        SettingsSandbox sandbox;
        Settings settings;
        // User has explicitly enabled the Process column in Settings.
        settings.setShowProcessColumn(true);

        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        // Simulate the in-process backend path (or an old agent that
        // doesn't advertise the wire token): empty capability list.
        w.setBackendInfo(false, QString(), QStringList{});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);
        const int processCol =
            static_cast<int>(ConnectionModel::Column::Process);
        QVERIFY2(connView->isColumnHidden(processCol),
                 "Process column visible despite no process-attribution-wire cap");

        // Now hand it the full capability list (production-equivalent)
        // and re-apply: the column must now show.
        w.setBackendInfo(true, QStringLiteral("0.5"),
                         QStringList{
                             QStringLiteral("process-attribution-wire"),
                             QStringLiteral("container-attribution-wire"),
                         });
        QTest::qWait(20);
        QVERIFY2(!connView->isColumnHidden(processCol),
                 "Process column still hidden after wire cap advertised");
    }

    // ---- batch 2 (scenario audit P0/P1 picks) ---------------------------

    // #1 filterExpressionTypedEndToEnd. Crosses: QLineEdit textChanged →
    // 200 ms debounce → ConnectionFilterProxy::setFilterExpression →
    // row visibility through GroupProxy → Settings persistence → the
    // text != persisted guard in applySettingsToUi that prevents a
    // feedback loop. Also pins: garbage expressions tint the bar red,
    // surface the parser error in the tooltip, and are NOT persisted.
    void filterExpressionTypedEndToEnd()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *edit     = w.findChild<QLineEdit*>(QStringLiteral("connFilterEdit"));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(edit);
        QVERIFY(connView);

        // The filter bar is toolbar-hosted and visibility-gated to the
        // Connections tab (updateConnIfaceFilterVisibility) — switch
        // there first so keyClicks reach a visible, enabled widget.
        w.selectConnectionsTab();
        QTest::qWait(20);
        QVERIFY(edit->isVisible());

        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
            mkFlow("eth0", "10.0.0.10", 1002, "8.8.8.8", 53,  L4Proto::Udp),
        });
        QTest::qWait(50);
        QCOMPARE(connView->model()->rowCount(), 2);

        // Type a valid expression; wait past the 200 ms debounce.
        QTest::keyClicks(edit, QStringLiteral("proto:tcp"));
        QTest::qWait(350);
        QCOMPARE(connView->model()->rowCount(), 1);
        // Valid expression must persist to Settings.
        QCOMPARE(settings.connectionFilterExpr(), QStringLiteral("proto:tcp"));
        // No error tint.
        QCOMPARE(edit->palette().color(QPalette::Base),
                 QPalette{}.color(QPalette::Base));

        // Replace with a parse error: red tint + tooltip, NOT persisted.
        edit->clear();
        QTest::keyClicks(edit, QStringLiteral("proto:(("));
        QTest::qWait(350);
        QVERIFY2(edit->palette().color(QPalette::Base)
                     != QPalette{}.color(QPalette::Base),
                 "error tint missing on parse failure");
        QVERIFY(edit->toolTip().contains(QStringLiteral("error"),
                                         Qt::CaseInsensitive));
        // Settings still hold the last VALID expression — clearing the
        // bar persisted "" first (clear() fires the debounce too), so
        // depending on timing we accept either "" or the prior valid
        // value; the invariant is the garbage itself is never stored.
        QVERIFY(settings.connectionFilterExpr() != QStringLiteral("proto:(("));

        // Clearing the bar restores all rows.
        edit->clear();
        QTest::qWait(350);
        QCOMPARE(connView->model()->rowCount(), 2);
        QCOMPARE(settings.connectionFilterExpr(), QString());
    }

    // #2 filterChangeWhileGroupedRestructuresWithoutReset. Filter
    // invalidation drives a DIFFERENT signal pattern into
    // ConnectionGroupProxy (layoutChanged / per-row removes from
    // QSortFilterProxyModel) than the rowsInserted/Removed path the
    // unit tests pin. Second front of bug-view-redraw: the surviving
    // group must stay expanded, the model must not reset, and the
    // emptied group must vanish + return.
    void filterChangeWhileGroupedRestructuresWithoutReset()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *edit     = w.findChild<QLineEdit*>(QStringLiteral("connFilterEdit"));
        auto *combo    = w.findChild<QComboBox*>(QStringLiteral("connViewModeCombo"));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(edit && combo && connView);

        connMon.emitSnapshot({
            mkFlow("eth0",  "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
            mkFlow("eth0",  "10.0.0.10", 1002, "1.0.0.1", 443, L4Proto::Tcp),
            mkFlow("wlan0", "10.0.0.11", 2001, "8.8.8.8", 53,  L4Proto::Udp),
        });
        QTest::qWait(50);

        const int byIface = combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByInterface));
        combo->setCurrentIndex(byIface);
        QTest::qWait(50);

        auto *m = connView->model();
        QCOMPARE(m->rowCount(), 2);             // eth0, wlan0
        // Find + expand the eth0 group (group order may vary).
        int ethRow = -1;
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, 0).data(Qt::DisplayRole)
                    .toString().contains(QStringLiteral("eth0"))) {
                ethRow = r;
                break;
            }
        }
        QVERIFY(ethRow >= 0);
        connView->expand(m->index(ethRow, 0));
        QVERIFY(connView->isExpanded(m->index(ethRow, 0)));

        QSignalSpy resetSpy(m, &QAbstractItemModel::modelReset);

        // Filter to TCP only → wlan0's lone UDP flow is filtered out,
        // its group must disappear; eth0 stays and stays expanded.
        edit->setText(QStringLiteral("proto:tcp"));
        QTest::qWait(350);
        QCOMPARE(m->rowCount(), 1);
        QVERIFY(m->index(0, 0).data(Qt::DisplayRole)
                    .toString().contains(QStringLiteral("eth0")));
        QVERIFY2(connView->isExpanded(m->index(0, 0)),
                 "eth0 group collapsed across filter application");

        // Clear the filter → wlan0 group returns.
        edit->clear();
        QTest::qWait(350);
        QCOMPARE(m->rowCount(), 2);

        // The whole dance must not have reset the model (a reset would
        // ALSO have collapsed the tree — assert both independently so a
        // failure pinpoints the mechanism).
        QCOMPARE(resetSpy.size(), 0);
    }

    // #3 dnsResolutionRerendersFlowColumn. Exact replica of the
    // bug-process-empty failure shape: data arrives asynchronously
    // AFTER the row was first painted. If the resolved() connection in
    // setDnsResolver or the targeted dataChanged in onResolved
    // regresses, the UI silently shows raw IPs forever while every
    // model unit test stays green.
    void dnsResolutionRerendersFlowColumn()
    {
        SettingsSandbox sandbox;
        Settings settings;
        settings.setResolveHostnames(true);

        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);

        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
        });
        QTest::qWait(50);

        auto *m = connView->model();
        QCOMPARE(m->rowCount(), 1);
        const int flowCol = static_cast<int>(ConnectionModel::Column::Flow);

        // The model must have ASKED the resolver (resolution enabled).
        QVERIFY2(dns.resolveCalls > 0, "model never called resolve()");
        // Pre-answer, the Flow column shows the raw address.
        QVERIFY(m->index(0, flowCol).data(Qt::DisplayRole)
                    .toString().contains(QStringLiteral("1.1.1.1")));

        // Async answer arrives: cache first (cachedName must return it),
        // then the resolved() signal that triggers the re-render.
        dns.setCached(QHostAddress(QStringLiteral("1.1.1.1")),
                      QStringLiteral("one.example"));
        dns.emitResolved(QHostAddress(QStringLiteral("1.1.1.1")),
                         QStringLiteral("one.example"));
        QTest::qWait(30);

        QVERIFY2(m->index(0, flowCol).data(Qt::DisplayRole)
                     .toString().contains(QStringLiteral("one.example")),
                 "Flow column did not re-render after async DNS answer");
    }

    // #5 pauseFreezesModelUpdatesAndResumeRecovers. The m_paused gate
    // is consulted in two independent consumers (onStatsUpdated and
    // onConnectionsUpdated); an inverted/missed gate in either is
    // invisible to unit tests. Also asserts the action's text flips so
    // the UI affordance tracks the state.
    void pauseFreezesModelUpdatesAndResumeRecovers()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *pause    = w.findChild<QAction*>(QStringLiteral("pauseAction"));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(pause && connView);

        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
        });
        QTest::qWait(30);
        QCOMPARE(connView->model()->rowCount(), 1);

        // Pause. New snapshot must NOT reach the model.
        pause->setChecked(true);
        QVERIFY(pause->text().contains(QStringLiteral("Resume")));
        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
            mkFlow("eth0", "10.0.0.10", 1002, "1.0.0.1", 443, L4Proto::Tcp),
        });
        QTest::qWait(30);
        QCOMPARE(connView->model()->rowCount(), 1);  // frozen

        // Resume. Next snapshot recovers.
        pause->setChecked(false);
        QVERIFY(pause->text().contains(QStringLiteral("Pause")));
        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
            mkFlow("eth0", "10.0.0.10", 1002, "1.0.0.1", 443, L4Proto::Tcp),
        });
        QTest::qWait(30);
        QCOMPARE(connView->model()->rowCount(), 2);  // recovered
    }

    // #11 staleRowLingersItalicThenPrunes. First coverage of any kind
    // for ConnectionModel's stale-row lifecycle: a flow absent from
    // the latest snapshot lingers (IsStaleRole true) for the retention
    // window, then is pruned. With retention 0 the prune happens on the
    // very next tick.
    void staleRowLingersThenPrunes()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);
        auto *m = connView->model();

        const auto flowA = mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443,
                                  L4Proto::Tcp);
        const auto flowB = mkFlow("eth0", "10.0.0.10", 1002, "1.0.0.1", 443,
                                  L4Proto::Tcp);

        connMon.emitSnapshot({flowA, flowB});
        QTest::qWait(30);
        QCOMPARE(m->rowCount(), 2);

        // Drop B from the snapshot — default retention (15 s) keeps the
        // row visible but stale.
        connMon.emitSnapshot({flowA});
        QTest::qWait(30);
        QCOMPARE(m->rowCount(), 2);
        int staleCount = 0;
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, 0).data(ConnectionModel::IsStaleRole).toBool())
                ++staleCount;
        }
        QCOMPARE(staleCount, 1);

        // Zero retention (both proto families) → B prunes on next tick.
        settings.setConnectionStaleRetentionSecs(0);
        settings.setConnectionStaleRetentionSecsUdp(0);
        QTest::qWait(20);  // applySettingsToUi delivery
        connMon.emitSnapshot({flowA});
        QTest::qWait(30);
        QCOMPARE(m->rowCount(), 1);
        QVERIFY(!m->index(0, 0).data(ConnectionModel::IsStaleRole).toBool());
    }

    // #6 permissionDeniedBannerShowsAndIsNotClobbered. The banner has
    // an explicit precedence guard: a later (less severe)
    // accountingUnavailable must NOT overwrite an EPERM banner that's
    // already showing. One inverted boolean away from regressing and
    // nothing else tests the banner at all.
    void permissionDeniedBannerShowsAndIsNotClobbered()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *banner = w.findChild<QFrame*>(QStringLiteral("connBanner"));
        auto *label  = w.findChild<QLabel*>(QStringLiteral("connBannerLabel"));
        QVERIFY(banner && label);
        // The banner lives inside the Connections tab; isVisible() is
        // false while another tab is current regardless of the banner's
        // own state. Switch first.
        w.selectConnectionsTab();
        QTest::qWait(20);
        QVERIFY(!banner->isVisible());

        connMon.emitPermissionDenied(QStringLiteral("Operation not permitted"));
        QTest::qWait(20);
        QVERIFY(banner->isVisible());
        QVERIFY(label->text().contains(QStringLiteral("CAP_NET_ADMIN")));

        // A later accounting hint must NOT clobber the EPERM banner.
        connMon.emitAccountingUnavailable(QStringLiteral("acct off"));
        QTest::qWait(20);
        QVERIFY(banner->isVisible());
        QVERIFY2(label->text().contains(QStringLiteral("CAP_NET_ADMIN")),
                 "accountingUnavailable clobbered the EPERM banner");

        // And the reverse order: accounting-only shows ITS banner when
        // nothing more severe was displayed. Fresh window for a clean
        // visibility slate.
        FakeConnectionMonitor connMon2;
        MainWindow w2(&settings, &netMon, &connMon2, &dns);
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        w2.selectConnectionsTab();
        QTest::qWait(20);
        auto *banner2 = w2.findChild<QFrame*>(QStringLiteral("connBanner"));
        auto *label2  = w2.findChild<QLabel*>(QStringLiteral("connBannerLabel"));
        connMon2.emitAccountingUnavailable(QStringLiteral("acct off"));
        QTest::qWait(20);
        QVERIFY(banner2->isVisible());
        QVERIFY(label2->text().contains(QStringLiteral("counters")));
    }

    // ---- batch 3 (scenario audit P1/P2 fully-feasible picks) ------------

    // #7 emptyStateOverlayTracksProxyRowCount. The overlay visibility is
    // driven by the FILTER proxy's rowsInserted/Removed/modelReset/
    // layoutChanged signals (NOT the group proxy). A filter that hides
    // every row should resurface the overlay — that path depends on
    // which signal invalidateFilter happens to emit, and nothing else
    // pins it.
    void emptyStateOverlayTracksProxyRowCount()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        // Overlay lives in the Connections tab viewport; isVisible() is
        // false while another tab is current regardless of state.
        w.selectConnectionsTab();
        QTest::qWait(20);

        auto *overlay = w.findChild<QLabel*>(QStringLiteral("connEmptyOverlay"));
        auto *edit    = w.findChild<QLineEdit*>(QStringLiteral("connFilterEdit"));
        QVERIFY(overlay && edit);

        // No flows yet → overlay visible.
        QVERIFY2(overlay->isVisible(), "overlay hidden with zero flows");

        // Flows arrive → overlay hidden.
        connMon.emitSnapshot({
            mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
        });
        QTest::qWait(30);
        QVERIFY2(!overlay->isVisible(), "overlay still visible with flows present");

        // A filter that matches nothing → overlay resurfaces.
        edit->setText(QStringLiteral("port=9999"));
        QTest::qWait(350);
        QVERIFY2(overlay->isVisible(),
                 "overlay did not resurface when filter hid every row");

        // Clearing the filter → overlay hides again.
        edit->clear();
        QTest::qWait(350);
        QVERIFY(!overlay->isVisible());
    }

    // #8 settingsChangeSyncsViewModeComboAndTreeDecorations. The
    // EXISTING smoke test covers the combo→Settings direction; this is
    // the reverse edge (Settings dialog → applySettingsToUi → group
    // proxy mode + tree decorations + combo back-sync via QSignalBlocker).
    // A missing QSignalBlocker would cause a re-entrant feedback loop.
    void settingsChangeSyncsViewModeComboAndTreeDecorations()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *combo    = w.findChild<QComboBox*>(QStringLiteral("connViewModeCombo"));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(combo && connView);

        // Default Flat: no decoration, zero indent.
        QCOMPARE(combo->currentIndex(),
                 static_cast<int>(Settings::ConnectionViewMode::Flat));
        QVERIFY(!connView->rootIsDecorated());
        QCOMPARE(connView->indentation(), 0);

        // Change mode through Settings (the dialog path) — combo must
        // follow, tree must gain decoration + nonzero indent.
        settings.setConnectionViewMode(Settings::ConnectionViewMode::ByInterface);
        QTest::qWait(30);
        QCOMPARE(combo->currentIndex(),
                 static_cast<int>(Settings::ConnectionViewMode::ByInterface));
        QVERIFY(connView->rootIsDecorated());
        QVERIFY(connView->indentation() > 0);

        // Back to Flat restores the v0.1 geometry.
        settings.setConnectionViewMode(Settings::ConnectionViewMode::Flat);
        QTest::qWait(30);
        QCOMPARE(combo->currentIndex(),
                 static_cast<int>(Settings::ConnectionViewMode::Flat));
        QCOMPARE(connView->indentation(), 0);
    }

    // #9 pollIntervalChangePropagatesDesiredCadenceToBackends. A poll-
    // interval change in Preferences must immediately push
    // setDesiredIntervalMs to BOTH monitors (the agent-liveness hint;
    // PERF-L2 family). The fakes already record lastDesiredMs.
    void pollIntervalChangePropagatesDesiredCadenceToBackends()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        settings.setPollIntervalMs(250);
        QTest::qWait(30);
        QCOMPARE(netMon.lastDesiredMs,  250);
        QCOMPARE(connMon.lastDesiredMs, 250);

        settings.setPollIntervalMs(2000);
        QTest::qWait(30);
        QCOMPARE(netMon.lastDesiredMs,  2000);
        QCOMPARE(connMon.lastDesiredMs, 2000);
    }

    // #10 ifaceFilterFromSettingsHidesRowsAndUpdatesButtonLabel. The
    // per-interface visibility filter fans out from Settings to the
    // proxy AND the toolbar button text. Unit tests cover the proxy
    // alone; the Settings→button label fan-out is unwireable without
    // anyone noticing.
    void ifaceFilterFromSettingsHidesRowsAndUpdatesButtonLabel()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        auto *btn      = w.findChild<QToolButton*>(QStringLiteral("connIfaceFilterBtn"));
        QVERIFY(connView && btn);

        connMon.emitSnapshot({
            mkFlow("eth0",  "10.0.0.10", 1001, "1.1.1.1", 443, L4Proto::Tcp),
            mkFlow("wlan0", "10.0.0.11", 2001, "8.8.8.8", 53,  L4Proto::Udp),
        });
        QTest::qWait(50);
        QCOMPARE(connView->model()->rowCount(), 2);
        QVERIFY(btn->text().contains(QStringLiteral("All")));

        // Restrict to eth0 → wlan0 row hidden, button shows the iface.
        settings.setConnectionVisibleIfaces({QStringLiteral("eth0")});
        QTest::qWait(30);
        QCOMPARE(connView->model()->rowCount(), 1);
        QCOMPARE(btn->text(), QStringLiteral("eth0"));

        // Clear → all rows return, label back to "All interfaces".
        settings.setConnectionVisibleIfaces({});
        QTest::qWait(30);
        QCOMPARE(connView->model()->rowCount(), 2);
        QVERIFY(btn->text().contains(QStringLiteral("All")));
    }

    // #17 gaugeColumnsFollowThroughputSetting. The RxMax/TxMax columns
    // are gauge-dependent (not user-controlled) — applySettingsToUi
    // force-toggles them based on throughputGaugeEnabled. Same AND-gate
    // family as the Process column but without the capability axis.
    void gaugeColumnsFollowThroughputSetting()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);
        const int rxMax = static_cast<int>(ConnectionModel::Column::RxMax);
        const int txMax = static_cast<int>(ConnectionModel::Column::TxMax);

        // Default OFF → both gauge columns hidden.
        QVERIFY(connView->isColumnHidden(rxMax));
        QVERIFY(connView->isColumnHidden(txMax));

        settings.setThroughputGaugeEnabled(true);
        QTest::qWait(30);
        QVERIFY2(!connView->isColumnHidden(rxMax), "RxMax hidden with gauge on");
        QVERIFY2(!connView->isColumnHidden(txMax), "TxMax hidden with gauge on");

        settings.setThroughputGaugeEnabled(false);
        QTest::qWait(30);
        QVERIFY(connView->isColumnHidden(rxMax));
        QVERIFY(connView->isColumnHidden(txMax));
    }
};

// We instantiate widgets, so QApplication is required (not
// QCoreApplication). QTEST_MAIN handles the platform plumbing —
// QT_QPA_PLATFORM=offscreen is set by the ctest invocation in CI.
QTEST_MAIN(TestMainWindowSmoke)
#include "test_mainwindow_smoke.moc"
