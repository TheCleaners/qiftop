// Settings: legacy display/rateSmoothingSecs ⇒ display/rateSmoothingMs migration.

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "config/Settings.h"
#include "backend/PlatformInfo.h"

namespace {

// Hermetic QSettings: redirect IniFormat/UserScope into a temp dir so
// the test never touches the user's real preferences.
void redirectSettings(const QString &dir)
{
    QCoreApplication::setOrganizationName("qiftop-test");
    QCoreApplication::setApplicationName("qiftop-test");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);
}

} // namespace

class TestSettingsMigration : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void legacy_secs_promoted_to_ms_and_removed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        // Seed legacy key.
        {
            QSettings s;
            s.setValue("display/rateSmoothingSecs", 3);
            s.sync();
        }

        Settings s;
        QCOMPARE(s.rateSmoothingMs(), 3000);

        // And the legacy key is gone.
        QSettings raw;
        QVERIFY(!raw.contains("display/rateSmoothingSecs"));
        QVERIFY(raw.contains("display/rateSmoothingMs"));
    }

    void modern_ms_key_wins_over_legacy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        {
            QSettings s;
            s.setValue("display/rateSmoothingMs", 250);
            s.setValue("display/rateSmoothingSecs", 99); // ignored
            s.sync();
        }

        Settings s;
        QCOMPARE(s.rateSmoothingMs(), 250);
    }

    void no_keys_means_default_zero()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        Settings s;
        QCOMPARE(s.rateSmoothingMs(), 0);
    }

    void chip_colors_roundtrip_and_reset()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        {
            Settings s;
            // Defaults present on a fresh store.
            QCOMPARE(s.chipColorPrimary(), Settings::defaultChipColorPrimary());

            s.setChipColorPrimary(QStringLiteral("#123456"));
            s.setChipColorUser(QStringLiteral("#abcdef"));
            // Invalid hex is rejected (value unchanged).
            const QString beforeId = s.chipColorId();
            s.setChipColorId(QStringLiteral("not-a-color"));
            QCOMPARE(s.chipColorId(), beforeId);
        }
        {
            // Persisted across reconstruction.
            Settings s;
            QCOMPARE(s.chipColorPrimary(), QStringLiteral("#123456"));
            QCOMPARE(s.chipColorUser(),    QStringLiteral("#abcdef"));

            // Reset restores all four defaults.
            s.resetChipColors();
            QCOMPARE(s.chipColorPrimary(), Settings::defaultChipColorPrimary());
            QCOMPARE(s.chipColorUser(),    Settings::defaultChipColorUser());
            QCOMPARE(s.chipColorId(),      Settings::defaultChipColorId());
            QCOMPARE(s.chipColorDetail(),  Settings::defaultChipColorDetail());
        }
        {
            // Reset persisted too.
            Settings s;
            QCOMPARE(s.chipColorPrimary(), Settings::defaultChipColorPrimary());
        }
    }

    // TESTS-#6: round-trip persistence for the v0.2 attribution view settings
    // (connection view mode + the capability-gated Process/Container column
    // toggles + container-chain-in-tooltip). None had coverage before.
    void attribution_view_settings_roundtrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        {
            // Defaults on a fresh store.
            Settings s;
            QCOMPARE(s.connectionViewMode(), Settings::ConnectionViewMode::Flat);
            QCOMPARE(s.showProcessColumn(),   false);
            QCOMPARE(s.showContainerColumn(), false);
            QCOMPARE(s.showContainerChainInTooltip(), true);

            s.setConnectionViewMode(Settings::ConnectionViewMode::ByContainer);
            s.setShowProcessColumn(true);
            s.setShowContainerColumn(true);
            s.setShowContainerChainInTooltip(false);
        }
        {
            // Persisted across reconstruction.
            Settings s;
            QCOMPARE(s.connectionViewMode(), Settings::ConnectionViewMode::ByContainer);
            QCOMPARE(s.showProcessColumn(),   true);
            QCOMPARE(s.showContainerColumn(), true);
            QCOMPARE(s.showContainerChainInTooltip(), false);
        }
    }

    // TESTS-#6: an out-of-range persisted view mode (e.g. written by a newer
    // build with more modes, or a corrupted store) must clamp to the Flat
    // default rather than yielding an invalid enum — see the load guard in
    // Settings.cpp.
    void connection_view_mode_out_of_range_clamps_to_flat()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        {
            QSettings raw;
            raw.setValue("connections/viewMode", 99); // beyond ByProcess(3)
            raw.sync();
        }
        Settings s;
        QCOMPARE(s.connectionViewMode(), Settings::ConnectionViewMode::Flat);

        // A negative value is equally invalid and must also clamp.
        {
            QSettings raw;
            raw.setValue("connections/viewMode", -1);
            raw.sync();
        }
        Settings s2;
        QCOMPARE(s2.connectionViewMode(), Settings::ConnectionViewMode::Flat);
    }

    // Unprivileged contract: settingsWriteWouldEscalate() must be false for a
    // normal (non-root) process, so persistence behaves exactly as before.
    // The positive (euid-0-foreign-home) branch needs root and is covered by
    // the live verification documented in the commit; here we pin that the
    // common path never trips the guard and that writes round-trip.
    void unprivileged_persistence_unaffected()
    {
        QVERIFY(!qiftop::platform::settingsWriteWouldEscalate());

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        redirectSettings(dir.path());

        {
            Settings s;
            s.setPollIntervalMs(4242);
        }
        Settings reopened;
        QCOMPARE(reopened.pollIntervalMs(), 4242);
    }
};

QTEST_MAIN(TestSettingsMigration)
#include "test_settings_migration.moc"
