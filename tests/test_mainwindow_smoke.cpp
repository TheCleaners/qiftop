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

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>

#include "config/Settings.h"
#include "ui/ConnectionModel.h"
#include "ui/SettingsDialog.h"
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

        // Grouping by process makes the Process column redundant (the value
        // lives in the group header), so it must hide even with cap + pref on;
        // switching away restores it.
        const int containerCol =
            static_cast<int>(ConnectionModel::Column::Container);
        settings.setConnectionViewMode(Settings::ConnectionViewMode::ByProcess);
        QTest::qWait(20);
        QVERIFY2(connView->isColumnHidden(processCol),
                 "Process column visible while grouped by process");
        QVERIFY2(!connView->isColumnHidden(containerCol),
                 "Container column hidden while grouped by process (should stay)");

        settings.setConnectionViewMode(Settings::ConnectionViewMode::ByContainer);
        QTest::qWait(20);
        QVERIFY2(connView->isColumnHidden(containerCol),
                 "Container column visible while grouped by container");
        QVERIFY2(!connView->isColumnHidden(processCol),
                 "Process column hidden while grouped by container (should stay)");

        settings.setConnectionViewMode(Settings::ConnectionViewMode::Flat);
        QTest::qWait(20);
        QVERIFY2(!connView->isColumnHidden(processCol),
                 "Process column not restored after leaving the process grouping");
        QVERIFY2(!connView->isColumnHidden(containerCol),
                 "Container column not restored after leaving the container grouping");
    }

    // The converse of the above, and the whole point of transport-neutral
    // capabilities: with an IN-PROCESS backend (usingAgent=false) that
    // advertises the attribution wire tokens — exactly what the BSD
    // BsdConnectionMonitor does — the Process/Container columns MUST show.
    // This proves the gate keys off the active backend's capability set, not
    // off agent presence: the old `usingAgent && caps.contains(...)` framing
    // is gone.
    void attributionColumnsShowOnInProcessBackendWithCaps()
    {
        SettingsSandbox sandbox;
        Settings settings;
        settings.setShowProcessColumn(true);
        settings.setShowContainerColumn(true);

        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        // usingAgent=false (in-process path) but the backend reports the
        // attribution tokens — like the in-process BSD backend.
        w.setBackendInfo(false, QString(),
                         QStringList{
                             QStringLiteral("process-attribution-wire"),
                             QStringLiteral("container-attribution-wire"),
                         });
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);
        const int processCol =
            static_cast<int>(ConnectionModel::Column::Process);
        const int containerCol =
            static_cast<int>(ConnectionModel::Column::Container);
        QVERIFY2(!connView->isColumnHidden(processCol),
                 "Process column hidden on in-process backend that advertises "
                 "process-attribution-wire (agent-only assumption still present?)");
        QVERIFY2(!connView->isColumnHidden(containerCol),
                 "Container column hidden on in-process backend that advertises "
                 "container-attribution-wire");
    }

    // The runtime attribution-eagerness toolbar combo is gated on the active
    // backend advertising `attribution-eagerness-hints`: hidden without it,
    // shown (and synced to the persisted choice) with it.
    void eagernessComboGatedOnCapability()
    {
        SettingsSandbox sandbox;
        Settings settings;
        settings.setAttributionEagerness(QStringLiteral("eager"));

        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.setBackendInfo(false, QString(), QStringList{}); // no eagerness cap
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *combo = w.findChild<QComboBox*>(QStringLiteral("attrEagernessCombo"));
        QVERIFY(combo);
        QVERIFY2(!combo->isVisible(),
                 "eagerness combo visible without attribution-eagerness-hints cap");

        w.setBackendInfo(true, QStringLiteral("0.7"),
                         QStringList{ QStringLiteral("attribution-eagerness-hints") });
        QTest::qWait(20);
        QVERIFY2(combo->isVisible(),
                 "eagerness combo hidden despite attribution-eagerness-hints cap");
        // Synced to the persisted "eager" choice.
        QCOMPARE(combo->currentData().toString(), QStringLiteral("eager"));
    }

    // Unattributed flows carry a server-side reason; the Process column must
    // render a colour-coded synthetic label ("— forwarded —", etc.) rather
    // than a bare dash, so router/NAT traffic isn't mistaken for an
    // attribution bug. Pins both the DisplayRole text and that a distinct
    // ForegroundRole colour is assigned.
    void unattributedFlowShowsColouredReasonLabel()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.setBackendInfo(true, QStringLiteral("0.6"),
                         QStringList{ QStringLiteral("process-attribution-wire"),
                                      QStringLiteral("attribution-reason") });
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);

        Connection fwd = mkFlow("eno1", "10.42.0.122", 9994,
                                "84.17.53.155", 9993, L4Proto::Udp, 100, 50);
        fwd.process.pid = 0;
        fwd.reason      = AttributionReason::Forwarded;
        Connection orph = mkFlow("eno1", "10.0.0.61", 54766,
                                 "18.155.202.125", 443, L4Proto::Tcp, 10, 10);
        orph.process.pid = 0;
        orph.reason      = AttributionReason::Orphaned;
        connMon.emitSnapshot({ fwd, orph });
        QTest::qWait(50);

        auto *m = connView->model();
        const int pcol = static_cast<int>(ConnectionModel::Column::Process);
        QCOMPARE(m->rowCount(), 2);

        // Locate the forwarded row by its ConnectionRole (sort order is not
        // asserted here) and check its Process cell.
        bool sawForwarded = false, sawOrphaned = false;
        for (int r = 0; r < m->rowCount(); ++r) {
            const auto c = m->index(r, 0)
                .data(ConnectionModel::ConnectionRole).value<Connection>();
            const QModelIndex p = m->index(r, pcol);
            const QString label = p.data(Qt::DisplayRole).toString();
            const QVariant fg   = p.data(Qt::ForegroundRole);
            if (c.reason == AttributionReason::Forwarded) {
                sawForwarded = true;
                QCOMPARE(label, QStringLiteral("— forwarded —"));
                QVERIFY2(fg.isValid() && fg.canConvert<QColor>(),
                         "forwarded reason label has no colour");
            } else if (c.reason == AttributionReason::Orphaned) {
                sawOrphaned = true;
                QCOMPARE(label, QStringLiteral("— orphaned —"));
                QVERIFY(fg.isValid() && fg.canConvert<QColor>());
            }
        }
        QVERIFY(sawForwarded);
        QVERIFY(sawOrphaned);
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
        // Keep the toolbar-hosted filter out of the overflow menu under
        // offscreen platforms with wider default metrics.
        w.resize(1400, 900);
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
        QTRY_VERIFY(edit->isVisible());

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

    // #8c viewModeMenuMirrorsComboAndSettings. The View → "Group
    // Connections" radio submenu must (a) expose exactly the four view
    // modes as checkable, exclusive actions, (b) drive
    // Settings::connectionViewMode when triggered, and (c) re-check the
    // correct action when the mode changes by any other path (combo /
    // Settings dialog). Guards the menu-bar mirror of the toolbar dropdown.
    void viewModeMenuMirrorsComboAndSettings()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *menu  = w.findChild<QMenu*>(QStringLiteral("connViewModeMenu"));
        auto *combo = w.findChild<QComboBox*>(QStringLiteral("connViewModeCombo"));
        QVERIFY(menu && combo);

        // Exactly the four modes, all checkable and mutually exclusive.
        const QList<QAction*> acts = menu->actions();
        QCOMPARE(acts.size(), 4);
        QList<int> modeData;
        for (QAction *a : acts) {
            QVERIFY(a->isCheckable());
            modeData << a->data().toInt();
        }
        std::sort(modeData.begin(), modeData.end());
        QCOMPARE(modeData, (QList<int>{
            static_cast<int>(Settings::ConnectionViewMode::Flat),
            static_cast<int>(Settings::ConnectionViewMode::ByInterface),
            static_cast<int>(Settings::ConnectionViewMode::ByContainer),
            static_cast<int>(Settings::ConnectionViewMode::ByProcess)}));

        auto actionFor = [&acts](Settings::ConnectionViewMode m) -> QAction* {
            for (QAction *a : acts)
                if (a->data().toInt() == static_cast<int>(m)) return a;
            return nullptr;
        };
        auto checkedCount = [&acts] {
            int n = 0;
            for (QAction *a : acts) if (a->isChecked()) ++n;
            return n;
        };

        // Default Flat is checked, and only it.
        QCOMPARE(checkedCount(), 1);
        QVERIFY(actionFor(Settings::ConnectionViewMode::Flat)->isChecked());

        // Triggering a menu action drives the setting + combo (menu→Settings).
        actionFor(Settings::ConnectionViewMode::ByContainer)->trigger();
        QTest::qWait(30);
        QCOMPARE(settings.connectionViewMode(),
                 Settings::ConnectionViewMode::ByContainer);
        QCOMPARE(combo->currentIndex(),
                 static_cast<int>(Settings::ConnectionViewMode::ByContainer));
        QCOMPARE(checkedCount(), 1);
        QVERIFY(actionFor(Settings::ConnectionViewMode::ByContainer)->isChecked());

        // Changing the mode by another path re-checks the right action
        // (Settings→menu sync) without leaving stale checks behind.
        settings.setConnectionViewMode(Settings::ConnectionViewMode::ByProcess);
        QTest::qWait(30);
        QCOMPARE(checkedCount(), 1);
        QVERIFY(actionFor(Settings::ConnectionViewMode::ByProcess)->isChecked());
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

    // ---- batch 4 (remaining feasible scenarios) -------------------------

    // #12 udpAggregateByPeerCollapsesEphemeralPorts. Three outbound UDP
    // flows from distinct ephemeral local ports to the SAME peer:53
    // collapse into one aggregated row (local port masked to *) when
    // the setting is on; toggling off splits them back to three.
    // Direction is set explicitly to Outbound so the test doesn't
    // depend on this host's ephemeral-port range (inferDirection).
    void udpAggregateByPeerCollapsesEphemeralPorts()
    {
        SettingsSandbox sandbox;
        Settings settings;
        settings.setUdpAggregateByPeer(true);
        // Zero stale retention so the aggregated row (local port masked
        // to 0) prunes immediately when we toggle aggregation off —
        // otherwise it lingers as a stale row and inflates the count.
        settings.setConnectionStaleRetentionSecs(0);
        settings.setConnectionStaleRetentionSecsUdp(0);
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);

        const auto udp = [](quint16 lport) {
            return mkFlow("eth0", "10.0.0.10", lport, "8.8.8.8", 53,
                          L4Proto::Udp, 100, 50, Direction::Outbound);
        };
        connMon.emitSnapshot({udp(40000), udp(40001), udp(40002)});
        QTest::qWait(50);
        // Aggregated: one row for the 8.8.8.8:53 peer.
        QCOMPARE(connView->model()->rowCount(), 1);

        // Turn aggregation off → three distinct ephemeral-port rows.
        settings.setUdpAggregateByPeer(false);
        QTest::qWait(20);
        connMon.emitSnapshot({udp(40000), udp(40001), udp(40002)});
        QTest::qWait(50);
        QCOMPARE(connView->model()->rowCount(), 3);
    }

    // #13 escClearsFilterAndCtrlFFocuses. Esc in the filter bar is a
    // WidgetShortcut (reliable offscreen once the widget has focus).
    // Ctrl+F is an ApplicationShortcut that switches to the Connections
    // tab and focuses the bar — that half needs an active window, so
    // it's gated on qWaitForWindowActive and skipped (not failed) when
    // the offscreen platform won't activate.
    void escClearsFilterAndCtrlFFocuses()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        // Keep the toolbar-hosted filter out of the overflow menu under
        // offscreen platforms with wider default metrics.
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.activateWindow();
        const bool activated = QTest::qWaitForWindowActive(&w, 500);
        Q_UNUSED(activated);
        w.selectConnectionsTab();

        auto *edit = w.findChild<QLineEdit*>(QStringLiteral("connFilterEdit"));
        QVERIFY(edit);
        QTRY_VERIFY(edit->isVisible());

        // Esc-clears half: give the bar focus, type, press Esc.
        edit->setFocus();
        QTRY_VERIFY(edit->hasFocus());
        QTest::keyClicks(edit, QStringLiteral("proto:tcp"));
        QTRY_COMPARE(edit->text(), QStringLiteral("proto:tcp"));
        QTest::keyClick(edit, Qt::Key_Escape);
        QTRY_COMPARE(edit->text(), QString());

        // Ctrl+F half (best-effort): only assert if the window actually
        // became active under the offscreen platform.
        w.activateWindow();
        if (QTest::qWaitForWindowActive(&w, 500)) {
            edit->clearFocus();
            // Switch to the Interfaces tab first so we can prove Ctrl+F
            // brings us back to Connections AND focuses the bar.
            if (auto *tabs = w.findChild<QTabWidget*>(QStringLiteral("mainTabs")))
                tabs->setCurrentIndex(0);
            QTest::qWait(20);
            QTest::keyClick(&w, Qt::Key_F, Qt::ControlModifier);
            QTest::qWait(20);
            QVERIFY(edit->hasFocus());
        } else {
            QWARN("offscreen platform did not activate window; "
                  "skipping Ctrl+F focus assertion");
        }
    }

    // #16 selectConnectionsTabSwitchesTab. The -i CLI path and the
    // Ctrl+2 shortcut both route through selectConnectionsTab(), a
    // public method. Drive it directly (no shortcut-activation caveat)
    // and assert the tab actually changed.
    void selectConnectionsTabSwitchesTab()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *tabs = w.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
        QVERIFY(tabs);

        // Default is the Interfaces tab (index 0).
        tabs->setCurrentIndex(0);
        QTest::qWait(20);
        QCOMPARE(tabs->currentIndex(), 0);

        w.selectConnectionsTab();
        QTest::qWait(20);
        QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("Connections"));
    }

    // #14 agentCadenceTintAndSuffixRoundTrip. notifyAgentCadence appends
    // a " — paused" / " — slowed (N ms)" suffix to the backend status
    // label and tints it; returning to nominal cadence strips both. The
    // suffix logic re-parses the label by searching for " — " — string
    // munging that silently corrupts if it ever drifts. setBackendInfo
    // must reset the tint.
    void agentCadenceTintAndSuffixRoundTrip()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.setBackendInfo(true, QStringLiteral("0.5"),
                         QStringList{QStringLiteral("cadence-signal")});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        auto *label = w.findChild<QLabel*>(QStringLiteral("statusBackend"));
        QVERIFY(label);
        const QString base = label->text();
        QVERIFY(base.contains(QStringLiteral("agent")));
        QVERIFY(label->styleSheet().isEmpty());

        // Paused: red tint + " — paused" suffix appended to the base.
        w.notifyAgentCadence(0);
        QVERIFY(label->text().startsWith(base));
        QVERIFY(label->text().contains(QStringLiteral("paused")));
        QVERIFY(!label->styleSheet().isEmpty());

        // Slowed: amber tint + "slowed (N ms)" suffix; base preserved
        // (the suffix from the prior call must be stripped, not stacked).
        w.notifyAgentCadence(2000);
        QVERIFY(label->text().startsWith(base));
        QVERIFY(label->text().contains(QStringLiteral("slowed")));
        QVERIFY(!label->text().contains(QStringLiteral("paused")));

        // Nominal: suffix + tint cleared, back to exactly the base text.
        w.notifyAgentCadence(1000);
        QCOMPARE(label->text(), base);
        QVERIFY(label->styleSheet().isEmpty());

        // setBackendInfo resets a leftover tint even mid-degradation.
        w.notifyAgentCadence(0);
        QVERIFY(!label->styleSheet().isEmpty());
        w.setBackendInfo(true, QStringLiteral("0.5"), QStringList{});
        QVERIFY(label->styleSheet().isEmpty());
    }

    // Reproduces the user-reported live-GUI bug: in ByProcess /
    // ByContainer modes the tree collapsed every few seconds because a
    // flow's group key changed when attribution resolved it (e.g.
    // pid 0 → pid 1234), and the old code reset the whole model on any
    // group-key change. This drives a real MainWindow: expand a group,
    // then deliver a snapshot where a previously-unattributed flow gains
    // a pid (new group key). The expanded group must STAY expanded and
    // the model must not reset.
    void groupedAttributionFlapKeepsTreeExpanded()
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

        // Two attributed flows (pid 1000 chrome) + one unattributed.
        auto chromeFlow = [](quint16 lport) {
            Connection c = mkFlow("eth0", "10.0.0.10", lport, "1.1.1.1", 443,
                                  L4Proto::Tcp, 1000, 500);
            c.process.pid  = 1000;
            c.process.comm = QStringLiteral("chrome");
            return c;
        };
        Connection pending = mkFlow("eth0", "10.0.0.10", 5555, "8.8.8.8", 443,
                                    L4Proto::Tcp, 100, 50);  // pid 0 → (unattributed)

        connMon.emitSnapshot({chromeFlow(1001), chromeFlow(1002), pending});
        QTest::qWait(50);

        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByProcess)));
        QTest::qWait(50);

        auto *m = connView->model();
        // chrome group + (unattributed) group.
        QCOMPARE(m->rowCount(), 2);
        // Find + expand the chrome group.
        int chromeRow = -1;
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, 0).data(Qt::DisplayRole)
                    .toString().contains(QStringLiteral("chrome"))) {
                chromeRow = r;
                break;
            }
        }
        QVERIFY(chromeRow >= 0);
        connView->expand(m->index(chromeRow, 0));
        QVERIFY(connView->isExpanded(m->index(chromeRow, 0)));

        QSignalSpy resetSpy(m, &QAbstractItemModel::modelReset);

        // Attribution resolves: the pending flow gains pid 1000 (chrome)
        // — its group key changes from "(unattributed)" to chrome's.
        // Simulate several ticks of this kind of flap.
        for (int tick = 0; tick < 5; ++tick) {
            Connection resolved = pending;
            resolved.process.pid  = 1000;
            resolved.process.comm = QStringLiteral("chrome");
            connMon.emitSnapshot({chromeFlow(1001), chromeFlow(1002), resolved});
            QTest::qWait(20);
            // And flap back to unattributed to stress the move both ways.
            connMon.emitSnapshot({chromeFlow(1001), chromeFlow(1002), pending});
            QTest::qWait(20);
        }

        QCOMPARE(resetSpy.size(), 0);   // NO collapse
        // The chrome group must still exist AND still be expanded.
        int chromeRow2 = -1;
        for (int r = 0; r < m->rowCount(); ++r) {
            if (m->index(r, 0).data(Qt::DisplayRole)
                    .toString().contains(QStringLiteral("chrome"))) {
                chromeRow2 = r;
                break;
            }
        }
        QVERIFY(chromeRow2 >= 0);
        QVERIFY2(connView->isExpanded(m->index(chromeRow2, 0)),
                 "chrome group collapsed across attribution flap");
    }

    // SettingsDialog uses a left category nav-list + stacked pages
    // (KiCad/VSCode style) rather than horizontal tabs. Pin: it
    // constructs, the nav list has the expected categories, and
    // selecting a row switches the stacked page.
    void settingsDialogSideNavSwitchesPages()
    {
        SettingsSandbox sandbox;
        Settings settings;
        SettingsDialog dlg(&settings, QStringList{});

        auto *nav = dlg.findChild<QListWidget*>(QStringLiteral("settingsNavList"));
        QVERIFY(nav);
        QVERIFY(nav->count() >= 4);   // Monitoring/Display/DNS/Tray (+more)
        auto *stack = dlg.findChild<QStackedWidget*>();
        QVERIFY(stack);
        QCOMPARE(stack->count(), nav->count());

        // Selecting a nav row switches the visible page.
        nav->setCurrentRow(0);
        QCOMPARE(stack->currentIndex(), 0);
        nav->setCurrentRow(2);
        QCOMPARE(stack->currentIndex(), 2);
    }

    // Companion to the flap test targeting the OTHER collapse cause: the
    // filter proxy is sorted by RxRate at startup, and with
    // dynamicSortFilter on it re-sorts on every source dataChanged —
    // rates change each tick, so it emitted layoutChanged every tick,
    // which the grouped handler turned into a full reset → collapse
    // every tick (independent of attribution). The flap test missed
    // this because its flows had CONSTANT rates. Here the rates churn
    // each tick; the group must stay expanded with zero resets.
    void groupedRateChurnDoesNotCollapseTree()
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

        auto flow = [](quint16 lport, quint64 rx) {
            return mkFlow("eth0", "10.0.0.10", lport, "1.1.1.1", 443,
                          L4Proto::Tcp, rx, rx / 2);
        };
        connMon.emitSnapshot({flow(1001, 1000), flow(1002, 2000), flow(1003, 500)});
        QTest::qWait(50);

        // Sort by RxTotal while still in Flat mode. This forwards through
        // the group proxy to the FILTER proxy (QSortFilterProxyModel),
        // setting ITS sort column. RxTotal's SortRole is the raw byte
        // count (changes immediately with each snapshot — unlike RxRate,
        // which is smoothed and wouldn't move within the test's time
        // budget). With dynamicSortFilter on, the filter proxy then
        // re-sorts on every snapshot, emitting layoutChanged — the
        // second collapse trigger this test targets.
        const int rxTotal = static_cast<int>(ConnectionModel::Column::RxTotal);
        connView->sortByColumn(rxTotal, Qt::DescendingOrder);
        QTest::qWait(20);

        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByInterface)));
        QTest::qWait(50);

        auto *m = connView->model();
        QCOMPARE(m->rowCount(), 1);          // single eth0 group
        connView->expand(m->index(0, 0));
        QVERIFY(connView->isExpanded(m->index(0, 0)));

        QSignalSpy resetSpy(m, &QAbstractItemModel::modelReset);

        // Churn byte totals every tick so the (would-be) sorted filter
        // proxy reorders and emits layoutChanged. With the fix the
        // source sort was cleared on entering grouped mode, so no
        // layoutChanged → no reset → the group stays expanded.
        for (int tick = 0; tick < 8; ++tick) {
            const quint64 base = 1000 + quint64(tick) * 1370;
            connMon.emitSnapshot({flow(1001, base),
                                  flow(1002, base * 3),
                                  flow(1003, base / 2 + 1)});
            QTest::qWait(20);
        }

        QCOMPARE(resetSpy.size(), 0);
        QVERIFY2(connView->isExpanded(m->index(0, 0)),
                 "eth0 group collapsed across rate churn");
    }

    // Increment-2 on-demand enrichment: expanding a ByProcess group
    // requests GetProcessDetails for that group's pid; when the reply
    // arrives the group tooltip gains exe/cmdline/cwd. Drives the real
    // wiring (MainWindow → group proxy → monitor) with the fake standing
    // in for the DBus RPC.
    void groupProcessDetailsFetchedOnExpand()
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

        Connection a = mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443,
                              L4Proto::Tcp, 1000, 500);
        a.process.pid  = 4242;
        a.process.comm = QStringLiteral("chrome");
        a.process.uid  = 0;
        connMon.emitSnapshot({a});
        QTest::qWait(50);

        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByProcess)));
        QTest::qWait(50);

        auto *m = connView->model();
        QCOMPARE(m->rowCount(), 1);
        const int flowCol = static_cast<int>(ConnectionModel::Column::Flow);

        // Expanding the group must request details for pid 4242.
        connView->expand(m->index(0, 0));
        QTest::qWait(20);
        QCOMPARE(connMon.lastRequestedPid, qint32(4242));
        QVERIFY(connMon.detailsRequests >= 1);

        // Before the reply, the tooltip has only wire fields.
        QVERIFY(!m->index(0, flowCol).data(Qt::ToolTipRole)
                     .toString().contains(QStringLiteral("Cmdline")));

        // Deliver the async reply → tooltip gains exe/cmdline/cwd.
        qiftop::backend::ProcessDetails d;
        d.pid     = 4242;
        d.uid     = 0;
        d.comm    = QStringLiteral("chrome");
        d.exe     = QStringLiteral("/usr/lib/chrome/chrome");
        d.cmdline = QStringLiteral("/usr/lib/chrome/chrome --type=renderer");
        d.cwd     = QStringLiteral("/home/ines");
        connMon.emitProcessDetails(d);
        QTest::qWait(20);

        const QString tip = m->index(0, flowCol).data(Qt::ToolTipRole).toString();
        QVERIFY2(tip.contains(QStringLiteral("Exe: /usr/lib/chrome/chrome")),
                 "tooltip missing exe after details reply");
        QVERIFY(tip.contains(QStringLiteral("Cmdline:")));
        QVERIFY(tip.contains(QStringLiteral("Cwd: /home/ines")));

        // A second expand of the same group must NOT re-request (cached).
        const int before = connMon.detailsRequests;
        connView->collapse(m->index(0, 0));
        connView->expand(m->index(0, 0));
        QTest::qWait(20);
        QCOMPARE(connMon.detailsRequests, before);
    }

    // M3: the on-demand details cache is keyed by pid; on PID reuse a
    // NEW process under a recycled pid must not show the DEAD process's
    // cached exe/cmdline/cwd. The staleness signal is comm mismatch
    // (cached entry's comm vs. the live flow's wire comm), and an
    // expand of the recycled group must re-request fresh details.
    void staleProcessDetailsIgnoredOnPidReuse()
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

        Connection a = mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443,
                              L4Proto::Tcp, 1000, 500);
        a.process.pid  = 4242;
        a.process.comm = QStringLiteral("chrome");
        connMon.emitSnapshot({a});
        QTest::qWait(50);

        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByProcess)));
        QTest::qWait(50);

        auto *m = connView->model();
        QCOMPARE(m->rowCount(), 1);
        const int flowCol = static_cast<int>(ConnectionModel::Column::Flow);

        // Warm the cache for pid 4242 (chrome).
        connView->expand(m->index(0, 0));
        QTest::qWait(20);
        qiftop::backend::ProcessDetails d;
        d.pid     = 4242;
        d.comm    = QStringLiteral("chrome");
        d.exe     = QStringLiteral("/usr/lib/chrome/chrome");
        d.cmdline = QStringLiteral("/usr/lib/chrome/chrome --type=renderer");
        d.cwd     = QStringLiteral("/home/ines");
        d.startTimeJiffies = 111;
        connMon.emitProcessDetails(d);
        QTest::qWait(20);
        QVERIFY(m->index(0, flowCol).data(Qt::ToolTipRole).toString()
                    .contains(QStringLiteral("Exe: /usr/lib/chrome/chrome")));

        // PID reuse: the same flow now belongs to a DIFFERENT process
        // that the kernel gave the recycled pid 4242 (comm "nginx").
        Connection b = a;
        b.process.comm = QStringLiteral("nginx");
        connMon.emitSnapshot({b});
        QTest::qWait(50);

        QCOMPARE(m->rowCount(), 1);
        const QString tip = m->index(0, flowCol).data(Qt::ToolTipRole).toString();
        QVERIFY2(!tip.contains(QStringLiteral("/usr/lib/chrome/chrome")),
                 "stale cached exe/cmdline shown for a recycled pid");
        QVERIFY(tip.contains(QStringLiteral("PID: 4242")));   // wire fields kept

        // The inline chips must not carry the stale cmdline either.
        const QVariantList chips =
            m->index(0, flowCol).data(ConnectionModel::GroupChipsRole).toList();
        for (const QVariant &v : chips) {
            QVERIFY(v.toMap().value(QStringLiteral("kind")).toString()
                        != QStringLiteral("cmdline"));
        }

        // Expanding the recycled group must evict the stale entry and
        // re-request details (the bare contains(pid) guard would skip).
        const int before = connMon.detailsRequests;
        connView->collapse(m->index(0, 0));
        connView->expand(m->index(0, 0));
        QTest::qWait(20);
        QCOMPARE(connMon.detailsRequests, before + 1);
        QCOMPARE(connMon.lastRequestedPid, qint32(4242));
    }

    // M2: tooltips are auto-detected as rich text by Qt; comm and
    // container fields are attacker-controlled (prctl(PR_SET_NAME),
    // image labels). Injected markup must arrive HTML-escaped inside a
    // deliberate <qt> wrapper — never as live tags (remote <img> loads
    // = surveillance beacon; spoofed tooltip content). Covers both the
    // ConnectionModel Process/Container ToolTipRole path and the
    // grouped header tooltip path.
    void tooltipRichTextInjectionEscaped()
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
        auto *m = connView->model();

        Connection a = mkFlow("eth0", "10.0.0.10", 1001, "1.1.1.1", 443,
                              L4Proto::Tcp, 1000, 500);
        a.process.pid       = 666;
        a.process.comm      = QStringLiteral("<img src=x>");
        a.container.runtime = QStringLiteral("docker");
        a.container.id      = QStringLiteral("abc123def4567890");
        a.container.name    = QStringLiteral("<b>evil</b>");
        connMon.emitSnapshot({a});
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);

        // --- ConnectionModel ToolTipRole path (flat mode) ---
        const int procCol = static_cast<int>(ConnectionModel::Column::Process);
        const int ctCol   = static_cast<int>(ConnectionModel::Column::Container);

        const QString procTip =
            m->index(0, procCol).data(Qt::ToolTipRole).toString();
        QVERIFY(procTip.startsWith(QStringLiteral("<qt>")));
        QVERIFY(procTip.contains(QStringLiteral("&lt;img")));
        QVERIFY(!procTip.contains(QStringLiteral("<img")));

        const QString ctTip =
            m->index(0, ctCol).data(Qt::ToolTipRole).toString();
        QVERIFY(ctTip.startsWith(QStringLiteral("<qt>")));
        QVERIFY(ctTip.contains(QStringLiteral("&lt;b&gt;evil&lt;/b&gt;")));
        QVERIFY(!ctTip.contains(QStringLiteral("<b>")));

        // --- Grouped header tooltip path (ByProcess) ---
        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByProcess)));
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);
        const int flowCol = static_cast<int>(ConnectionModel::Column::Flow);
        const QString grpTip =
            m->index(0, flowCol).data(Qt::ToolTipRole).toString();
        QVERIFY(grpTip.startsWith(QStringLiteral("<qt>")));
        QVERIFY(grpTip.contains(QStringLiteral("&lt;img")));
        QVERIFY(!grpTip.contains(QStringLiteral("<img")));

        // --- Grouped header tooltip path (ByContainer) ---
        combo->setCurrentIndex(combo->findData(
            static_cast<int>(Settings::ConnectionViewMode::ByContainer)));
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);
        const QString grpCtTip =
            m->index(0, flowCol).data(Qt::ToolTipRole).toString();
        QVERIFY(grpCtTip.startsWith(QStringLiteral("<qt>")));
        QVERIFY(grpCtTip.contains(QStringLiteral("&lt;b&gt;evil&lt;/b&gt;")));
        QVERIFY(!grpCtTip.contains(QStringLiteral("<b>")));
    }

    // M6: with UDP peer-aggregation on, an individual member conntrack
    // entry expiring (dropping out of the snapshot) must NOT subtract
    // its accumulated bytes from the aggregate row — totals are
    // monotone, fed from retained per-member state. Also covers the
    // member counter-reset (conntrack tuple recycling) fold.
    void udpAggregateRetainsBytesWhenMemberExpires()
    {
        SettingsSandbox sandbox;
        Settings settings;
        settings.setUdpAggregateByPeer(true);
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *connView = w.findChild<QTreeView*>(QStringLiteral("connView"));
        QVERIFY(connView);
        auto *m = connView->model();

        const auto udp = [](quint16 lport, quint64 rx, quint64 tx) {
            return mkFlow("eth0", "10.0.0.10", lport, "8.8.8.8", 53,
                          L4Proto::Udp, rx, tx, Direction::Outbound);
        };
        const auto aggConn = [&]() {
            return m->index(0, 0).data(ConnectionModel::ConnectionRole)
                       .value<Connection>();
        };

        // Two members → one aggregate row totalling 200/100.
        connMon.emitSnapshot({udp(40000, 100, 50), udp(40001, 100, 50)});
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);
        QCOMPARE(aggConn().rxBytes, quint64(200));
        QCOMPARE(aggConn().txBytes, quint64(100));

        // Member 40001 expires from conntrack while 40000 keeps
        // growing. The aggregate must KEEP 40001's 100/50 → 250/120,
        // not regress to 150/70.
        connMon.emitSnapshot({udp(40000, 150, 70)});
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);
        QCOMPARE(aggConn().rxBytes, quint64(250));
        QCOMPARE(aggConn().txBytes, quint64(120));

        // Member 40000's tuple is recycled (counters reset below the
        // high-water mark): old 150/70 folds into the base →
        // 150+30+100 = 280 / 70+10+50 = 130. Still monotone.
        connMon.emitSnapshot({udp(40000, 30, 10)});
        QTest::qWait(50);
        QCOMPARE(m->rowCount(), 1);
        QCOMPARE(aggConn().rxBytes, quint64(280));
        QCOMPARE(aggConn().txBytes, quint64(130));
    }

    // PERF-L2: while the window is hidden to tray the agent cadence heartbeat
    // must be SUSPENDED (no SetDesiredIntervalMs pushes), so the agent can
    // wind down its expensive conntrack+attribution polling. Showing the
    // window must immediately re-assert the cadence so the agent wakes.
    //
    // We assert this via push-suppression (deterministic, no waiting for a
    // 4 s heartbeat tick): a poll-interval change made WHILE HIDDEN must not
    // reach the backends; the next show() must flush the current cadence.
    void hiddenWindowSuspendsAgentHeartbeatAndShowResumes()
    {
        SettingsSandbox sandbox;
        Settings settings;
        FakeNetworkMonitor    netMon;
        FakeConnectionMonitor connMon;
        FakeDnsResolver       dns;

        MainWindow w(&settings, &netMon, &connMon, &dns);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        // Visible: a cadence change reaches both backends.
        settings.setPollIntervalMs(250);
        QTest::qWait(30);
        QCOMPARE(netMon.lastDesiredMs,  250);
        QCOMPARE(connMon.lastDesiredMs, 250);

        // Hide to tray. isVisible() is false; the heartbeat is suspended.
        w.hide();
        QTest::qWait(30);
        QVERIFY(!w.isVisible());

        // A cadence change made while hidden must NOT be pushed — the gate in
        // refreshAgentHeartbeat() short-circuits before asserting.
        settings.setPollIntervalMs(1750);
        QTest::qWait(30);
        QCOMPARE(netMon.lastDesiredMs,  250);   // unchanged
        QCOMPARE(connMon.lastDesiredMs, 250);   // unchanged

        // Showing the window re-asserts the CURRENT cadence immediately.
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::qWait(30);
        QCOMPARE(netMon.lastDesiredMs,  1750);
        QCOMPARE(connMon.lastDesiredMs, 1750);
    }
};

// We instantiate widgets, so QApplication is required (not
// QCoreApplication). QTEST_MAIN handles the platform plumbing —
// QT_QPA_PLATFORM=offscreen is set by the ctest invocation in CI.
QTEST_MAIN(TestMainWindowSmoke)
#include "test_mainwindow_smoke.moc"
