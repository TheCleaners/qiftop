// Unit tests for qiftop::agent::AttributionHintManager.
//
// Pure-logic coverage of the runtime attribution-eagerness override: the
// three-rule effective-mode computation, config-off uncancellability,
// most-eager-wins, per-sender keying + clear, TTL expiry, the 64-entry cap,
// and NameOwnerChanged cleanup. No DBus bus required — same pattern as
// test_idle drives IdleManager. Timers are scaled down so the suite stays
// well under a second.

#include <QSignalSpy>
#include <QTest>

#include "agent/AttributionHintManager.h"
#include "backend/ProcessResolver.h"

using qiftop::agent::AttributionHintManager;
using Eagerness = qiftop::backend::AttributionEagerness;

class TestAttributionHints : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<qiftop::backend::AttributionEagerness>();
    }

    void configDefaultUsedWhenNoHints()
    {
        AttributionHintManager balanced(Eagerness::Balanced);
        QCOMPARE(balanced.effectiveMode(), Eagerness::Balanced);
        QCOMPARE(balanced.effectiveModeString(), QStringLiteral("balanced"));

        AttributionHintManager eager(Eagerness::Eager);
        QCOMPARE(eager.effectiveMode(), Eagerness::Eager);
        QCOMPARE(eager.effectiveModeString(), QStringLiteral("eager"));
    }

    void mostEagerHintWins()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        QVERIFY(mgr.setHint(QStringLiteral(":1.10"), Eagerness::Off));
        QVERIFY(mgr.setHint(QStringLiteral(":1.11"), Eagerness::Eager));
        QVERIFY(mgr.setHint(QStringLiteral(":1.12"), Eagerness::Balanced));
        // eager > balanced > off — the loudest active hint wins.
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
    }

    void loneHintCanLowerConfigEager()
    {
        // Config default eager; a single client asking for balanced lowers
        // the effective mode (config default does NOT participate when any
        // live hint exists).
        AttributionHintManager mgr(Eagerness::Eager);
        QVERIFY(mgr.setHint(QStringLiteral(":1.42"), Eagerness::Balanced));
        QCOMPARE(mgr.effectiveMode(), Eagerness::Balanced);
    }

    void configOffIsUncancellable()
    {
        AttributionHintManager mgr(Eagerness::Off);
        QSignalSpy spy(&mgr, &AttributionHintManager::effectiveModeChanged);
        // Even an explicit eager hint can't re-enable attribution that the
        // config kill switch turned off.
        mgr.setHint(QStringLiteral(":1.1"), Eagerness::Eager);
        mgr.setHint(QStringLiteral(":1.2"), Eagerness::Balanced);
        QCOMPARE(mgr.effectiveMode(), Eagerness::Off);
        // No transition ever happened, so no signal.
        QCOMPARE(spy.count(), 0);
    }

    void clearViaDefaultRevertsToConfig()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        QVERIFY(mgr.setHint(QStringLiteral(":1.42"), Eagerness::Eager));
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
        QVERIFY(mgr.clearHint(QStringLiteral(":1.42")));
        QCOMPARE(mgr.effectiveMode(), Eagerness::Balanced);
    }

    void perSenderKeying()
    {
        // Two senders; clearing one leaves the other's hint in force.
        AttributionHintManager mgr(Eagerness::Balanced);
        mgr.setHint(QStringLiteral(":1.1"), Eagerness::Eager);
        mgr.setHint(QStringLiteral(":1.2"), Eagerness::Off);
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
        mgr.clearHint(QStringLiteral(":1.1"));
        // :1.2 still active at off → effective off (most eager of the
        // remaining hints, config default ignored).
        QCOMPARE(mgr.effectiveMode(), Eagerness::Off);
    }

    void effectiveModeChangedFiresOnTransitionsOnly()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        QSignalSpy spy(&mgr, &AttributionHintManager::effectiveModeChanged);

        mgr.setHint(QStringLiteral(":1.1"), Eagerness::Eager);   // balanced→eager
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<Eagerness>(), Eagerness::Eager);

        // A second, less-eager hint doesn't change the effective mode.
        mgr.setHint(QStringLiteral(":1.2"), Eagerness::Balanced);
        QCOMPARE(spy.count(), 0);

        mgr.clearHint(QStringLiteral(":1.1"));                   // eager→balanced
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<Eagerness>(), Eagerness::Balanced);
    }

    void ttlExpiryLowersModeAndFires()
    {
        // Small TTL so the periodic prune (half-TTL) fires within the test.
        AttributionHintManager mgr(Eagerness::Balanced, /*hintTtlMs=*/120);
        QSignalSpy spy(&mgr, &AttributionHintManager::effectiveModeChanged);
        mgr.setHint(QStringLiteral(":1.42"), Eagerness::Eager);
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
        QCOMPARE(spy.count(), 1);

        // Let the hint lapse; the prune timer should notice and drop back to
        // the config default, firing the change WITHOUT a fresh inbound call.
        QTRY_COMPARE_WITH_TIMEOUT(mgr.effectiveMode(), Eagerness::Balanced, 1000);
        bool sawBalanced = false;
        for (const auto &args : spy)
            if (args.at(0).value<Eagerness>() == Eagerness::Balanced) sawBalanced = true;
        QVERIFY2(sawBalanced, "expected an effectiveModeChanged(balanced) on TTL expiry");
    }

    void hintTableCapRejectsBeyond64()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        for (int i = 0; i < 64; ++i)
            QVERIFY(mgr.setHint(QStringLiteral(":1.%1").arg(i), Eagerness::Balanced));
        const Eagerness before = mgr.effectiveMode();
        // 65th new sender rejected (reject, not evict) even though it would
        // have raised the effective mode.
        QVERIFY(!mgr.setHint(QStringLiteral(":1.attacker"), Eagerness::Eager));
        QCOMPARE(mgr.effectiveMode(), before);
        // An existing sender can still update in place.
        QVERIFY(mgr.setHint(QStringLiteral(":1.0"), Eagerness::Eager));
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
    }

    void emptySenderRejected()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        QVERIFY(!mgr.setHint(QString(), Eagerness::Eager));
        QVERIFY(!mgr.clearHint(QString()));
        QCOMPARE(mgr.effectiveMode(), Eagerness::Balanced);
    }

    void nameOwnerChangedDropsHint()
    {
        AttributionHintManager mgr(Eagerness::Balanced);
        mgr.setHint(QStringLiteral(":1.42"), Eagerness::Eager);
        QCOMPARE(mgr.effectiveMode(), Eagerness::Eager);
        // Drive the private slot directly — same call path the bus
        // subscription uses. Old owner non-empty, new owner empty == gone.
        const bool ok = QMetaObject::invokeMethod(
            &mgr, "onNameOwnerChanged", Qt::DirectConnection,
            Q_ARG(QString, QStringLiteral(":1.42")),
            Q_ARG(QString, QStringLiteral(":1.42")),
            Q_ARG(QString, QString()));
        QVERIFY(ok);
        QCOMPARE(mgr.effectiveMode(), Eagerness::Balanced);
    }
};

QTEST_MAIN(TestAttributionHints)
#include "test_attribution_hints.moc"
