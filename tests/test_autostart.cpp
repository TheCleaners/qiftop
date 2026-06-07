// XDG autostart entry write/read/remove. Filesystem-isolated via
// XDG_CONFIG_HOME pointed at a QTemporaryDir.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "util/Autostart.h"

class TestAutostart : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        // Point XDG_CONFIG_HOME at the temp dir. We deliberately do NOT
        // call QStandardPaths::setTestModeEnabled(true) — it redirects
        // GenericConfigLocation to a per-app sandbox under
        // ~/.cache/qttest/ that ignores XDG_CONFIG_HOME, which would
        // both bypass our override and leak state across test runs.
        qputenv("XDG_CONFIG_HOME", m_dir.path().toUtf8());
    }

    void init()
    {
        // Each test starts with no autostart entry. Use the path the
        // production code computes so the cleanup tracks any future
        // location changes automatically.
        QFile::remove(qiftop::autostart::entryPath());
    }

    void disabled_by_default()
    {
        QVERIFY(!qiftop::autostart::isEnabled());
        QVERIFY(!QFile::exists(qiftop::autostart::entryPath()));
    }

    void enable_writes_a_valid_entry()
    {
        QVERIFY(qiftop::autostart::setEnabled(true));
        QVERIFY(QFile::exists(qiftop::autostart::entryPath()));
        QVERIFY(qiftop::autostart::isEnabled());

        QFile f(qiftop::autostart::entryPath());
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString body = QString::fromUtf8(f.readAll());
        QVERIFY(body.contains("[Desktop Entry]"));
        QVERIFY(body.contains("Type=Application"));
        QVERIFY(body.contains("--tray"));
        QVERIFY(body.contains("X-GNOME-Autostart-enabled=true"));
        QVERIFY(body.contains("Hidden=false"));
    }

    void disable_removes_the_entry()
    {
        QVERIFY(qiftop::autostart::setEnabled(true));
        QVERIFY(QFile::exists(qiftop::autostart::entryPath()));
        QVERIFY(qiftop::autostart::setEnabled(false));
        QVERIFY(!QFile::exists(qiftop::autostart::entryPath()));
        QVERIFY(!qiftop::autostart::isEnabled());
    }

    void disable_when_absent_is_noop_success()
    {
        QVERIFY(qiftop::autostart::setEnabled(false));
        QVERIFY(!qiftop::autostart::isEnabled());
    }

    void hidden_true_counts_as_disabled()
    {
        QVERIFY(qiftop::autostart::setEnabled(true));
        // Externally flip Hidden=true (simulates user disabling via
        // gnome-tweaks etc.) and verify we report disabled.
        const QString p = qiftop::autostart::entryPath();
        QFile f(p);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QString body = QString::fromUtf8(f.readAll());
        f.close();
        body.replace("Hidden=false", "Hidden=true");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        f.write(body.toUtf8());
        f.close();

        QVERIFY(!qiftop::autostart::isEnabled());
    }
};

QTEST_MAIN(TestAutostart)
#include "test_autostart.moc"
