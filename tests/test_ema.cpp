// dt-aware EMA. Pure.

#include <QtTest/QtTest>

#include "ui/ConnectionHeuristics.h"

using qiftop::heuristics::emaUpdate;

class TestEma : public QObject {
    Q_OBJECT
private slots:
    void tau_zero_returns_raw()
    {
        // τ=0 ⇒ no smoothing.
        QCOMPARE(emaUpdate(0.0,  100.0, 1000.0, 0.0), 100.0);
        QCOMPARE(emaUpdate(99.0,  10.0, 1000.0, 0.0),  10.0);
    }

    void zero_dt_returns_raw()
    {
        // Δt≤0 ⇒ treat first sample of a series as raw (prevents the
        // initial reading from blending into a stale zero).
        QCOMPARE(emaUpdate(50.0, 200.0, 0.0,   500.0), 200.0);
        QCOMPARE(emaUpdate(50.0, 200.0, -10.0, 500.0), 200.0);
    }

    void large_tau_keeps_prev_almost_unchanged()
    {
        // τ ≫ Δt ⇒ α tiny ⇒ output ≈ prev.
        const double out = emaUpdate(100.0, 0.0, /*dt=*/10.0, /*tau=*/100000.0);
        QVERIFY(out > 99.9 && out < 100.0);
    }

    void converges_to_constant_input()
    {
        // Feed a constant for many steps; output asymptotes to it.
        double s = 0.0;
        for (int i = 0; i < 200; ++i)
            s = emaUpdate(s, /*raw=*/42.0, /*dt=*/100.0, /*tau=*/500.0);
        QVERIFY(qAbs(s - 42.0) < 1e-6);
    }

    void dt_invariance_one_step_vs_two_halfsteps_close()
    {
        // Single Δt step ≈ two Δt/2 steps for the same continuous input.
        // Not exactly equal (EMA is discrete), but within a tight bound.
        const double prev = 10.0;
        const double raw  = 100.0;
        const double tau  = 1000.0;

        const double oneStep = emaUpdate(prev, raw, 200.0, tau);
        double twoStep = emaUpdate(prev,  raw, 100.0, tau);
        twoStep        = emaUpdate(twoStep, raw, 100.0, tau);

        QVERIFY(qAbs(oneStep - twoStep) < 1.0);
    }

    void alpha_formula_spot_check()
    {
        // α = Δt/(τ+Δt) = 1000/(1000+1000) = 0.5 ⇒ midpoint between prev and raw.
        const double out = emaUpdate(0.0, 100.0, 1000.0, 1000.0);
        QCOMPARE(out, 50.0);
    }

    void ease_out_cubic_endpoints_and_midpoint()
    {
        using qiftop::heuristics::easeOutCubic;
        QCOMPARE(easeOutCubic(0.0), 0.0);
        QCOMPARE(easeOutCubic(1.0), 1.0);
        // Midpoint: 1 - (0.5)^3 = 0.875 — fast early progress.
        QVERIFY(qAbs(easeOutCubic(0.5) - 0.875) < 1e-9);
        // Monotonic.
        QVERIFY(easeOutCubic(0.25) < easeOutCubic(0.75));
    }
};

QTEST_APPLESS_MAIN(TestEma)
#include "test_ema.moc"
