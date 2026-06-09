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

#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
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
};

// We instantiate widgets, so QApplication is required (not
// QCoreApplication). QTEST_MAIN handles the platform plumbing —
// QT_QPA_PLATFORM=offscreen is set by the ctest invocation in CI.
QTEST_MAIN(TestMainWindowSmoke)
#include "test_mainwindow_smoke.moc"
