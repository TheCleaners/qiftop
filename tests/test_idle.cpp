// Unit tests for qiftop::agent::IdleManager.
//
// Uses tiny in-test fakes for NetworkMonitor / ConnectionMonitor so the
// test never touches netlink or DBus. Times are scaled down to ~100 ms
// windows so the full test suite stays under a second.

#include <QDBusConnection>
#include <QSignalSpy>
#include <QTest>

#include "agent/IdleManager.h"
#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"

namespace {

class FakeNet : public NetworkMonitor {
public:
    using NetworkMonitor::NetworkMonitor;
    void start() override {}
    void stop()  override {}
    void setPollIntervalMs(int ms) override { lastInterval = ms; ++setCount; }
    int  lastInterval = -1;
    int  setCount     = 0;
};

class FakeConn : public ConnectionMonitor {
public:
    using ConnectionMonitor::ConnectionMonitor;
    void start() override {}
    void stop()  override {}
    void setPollIntervalMs(int ms) override { lastInterval = ms; ++setCount; }
    int  lastInterval = -1;
    int  setCount     = 0;
};

qiftop::agent::IdleManager::Config fastConfig()
{
    // Aggressively scaled-down windows so the test runs in well under a
    // second while still exercising every threshold.
    return {
        .activeIntervalMs = 100,
        .slow1IntervalMs  = 200,
        .slow2IntervalMs  = 400,
        .activeWindowMs   = 150,
        .slow1WindowMs    = 300,
        .slow2WindowMs    = 500,
        .idleTimeoutMs    = 500,
        .minIntervalMs    = 50,
        .hintTtlMs        = 200,
    };
}

} // namespace

class TestIdle : public QObject {
    Q_OBJECT

private slots:
    void appliesActiveIntervalImmediatelyOnConstruction()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        QCOMPARE(idle.currentIntervalMs(), 100);
        QCOMPARE(net.lastInterval, 100);
        QCOMPARE(conn.lastInterval, 100);
    }

    void cadenceChangedFiresOnApply()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        QSignalSpy spy(&idle, &qiftop::agent::IdleManager::cadenceChanged);
        // The constructor already emitted once; subsequent activity that
        // doesn't change the rate must NOT re-emit.
        idle.noteActivity();
        QCOMPARE(spy.count(), 0);
        // A hint at a faster rate should change it and fire once.
        idle.setClientHint(QStringLiteral(":1.42"), 50);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toInt(), 50);
    }

    void clientHintClampsToMinInterval()
    {
        FakeNet net; FakeConn conn;
        auto cfg = fastConfig();
        qiftop::agent::IdleManager idle(&net, &conn, cfg);
        idle.setClientHint(QStringLiteral(":1.42"), 1); // way below floor
        QCOMPARE(idle.currentIntervalMs(), cfg.minIntervalMs);
    }

    void minAcrossHintsWins()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        idle.setClientHint(QStringLiteral(":1.10"), 300);
        idle.setClientHint(QStringLiteral(":1.11"), 80);
        idle.setClientHint(QStringLiteral(":1.12"), 200);
        QCOMPARE(idle.currentIntervalMs(), 80);
    }

    void hintExpiresAfterTtl()
    {
        FakeNet net; FakeConn conn;
        auto cfg = fastConfig();
        cfg.hintTtlMs = 80;
        qiftop::agent::IdleManager idle(&net, &conn, cfg);
        idle.setClientHint(QStringLiteral(":1.42"), 50);
        QCOMPARE(idle.currentIntervalMs(), 50);

        // Wait for the hint to lapse, then trigger evaluation by sending
        // any other hint (or noteActivity, which calls effectiveActive…).
        QTest::qWait(cfg.hintTtlMs + 50);
        idle.noteActivity();
        QCOMPARE(idle.currentIntervalMs(), cfg.activeIntervalMs);
    }

    void clearedHintRevertsToBaseline()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        idle.setClientHint(QStringLiteral(":1.42"), 50);
        QCOMPARE(idle.currentIntervalMs(), 50);
        idle.setClientHint(QStringLiteral(":1.42"), 0); // ms<=0 clears
        QCOMPARE(idle.currentIntervalMs(), 100);
    }

    void hintTableCapRejectsBeyond64()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        // Fill to the documented cap with progressively stricter hints.
        for (int i = 0; i < 64; ++i)
            idle.setClientHint(QStringLiteral(":1.%1").arg(i),
                               100 - (i % 50)); // always >= minInterval
        const int beforeMs = idle.currentIntervalMs();
        // 65th hint MUST be rejected, even if it would have lowered the
        // effective interval — rejecting (not evicting) is what stops a
        // hostile peer from kicking out legitimate clients' hints.
        idle.setClientHint(QStringLiteral(":1.attacker"), 50);
        QCOMPARE(idle.currentIntervalMs(), beforeMs);
    }

    void degradesAndPausesOverTime()
    {
        FakeNet net; FakeConn conn;
        auto cfg = fastConfig();
        qiftop::agent::IdleManager idle(&net, &conn, cfg);
        QSignalSpy spy(&idle, &qiftop::agent::IdleManager::cadenceChanged);

        // Wait long enough to cross slow1Window. The 1 s evaluation timer
        // is what triggers transitions in the absence of activity, but the
        // *first* tick fires at 1 s regardless of our scaled-down windows.
        // So we wait > 1 s here to make sure evaluate() has run after the
        // window thresholds were crossed.
        QTest::qWait(1100);

        // After idleTimeout (500 ms ago, plus the 1 s evaluation grace),
        // the manager should have paused: current interval == 0.
        QCOMPARE(idle.currentIntervalMs(), 0);
        QCOMPARE(net.lastInterval, 0);
        QCOMPARE(conn.lastInterval, 0);
        // And cadenceChanged should have fired at least once with ms==0.
        bool sawPause = false;
        for (const auto &args : spy) {
            if (args.first().toInt() == 0) { sawPause = true; break; }
        }
        QVERIFY2(sawPause, "expected at least one cadenceChanged(0) emission");

        // Any incoming activity should immediately restore the active rate.
        idle.noteActivity();
        QCOMPARE(idle.currentIntervalMs(), cfg.activeIntervalMs);
    }

    void nameOwnerChangedDropsHint()
    {
        FakeNet net; FakeConn conn;
        qiftop::agent::IdleManager idle(&net, &conn, fastConfig());
        idle.setClientHint(QStringLiteral(":1.42"), 50);
        QCOMPARE(idle.currentIntervalMs(), 50);

        // Invoke the private slot directly by name — same call path the
        // bus signal subscription would use. Old owner non-empty, new
        // owner empty == "peer disconnected from bus".
        const bool ok = QMetaObject::invokeMethod(
            &idle, "onNameOwnerChanged", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral(":1.42")),
            Q_ARG(QString, QStringLiteral(":1.42")),
            Q_ARG(QString, QString()));
        QVERIFY(ok);
        QCOMPARE(idle.currentIntervalMs(), 100);
    }
};

QTEST_MAIN(TestIdle)
#include "test_idle.moc"
