// Settings: legacy display/rateSmoothingSecs ⇒ display/rateSmoothingMs migration.

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "config/Settings.h"

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
};

QTEST_MAIN(TestSettingsMigration)
#include "test_settings_migration.moc"
