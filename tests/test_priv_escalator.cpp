// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for util::PrivilegeEscalator::envAllowlist() / filterEnv().
//
// SECURITY: this is the gate that decides which env vars are forwarded
// into a privileged child (root). If a regression accidentally adds
// `LD_PRELOAD` or similar to the allowlist, a local attacker can inject
// arbitrary code into the root process via the self-elevation path.
// These tests pin down the dangerous keys explicitly so any change to
// the allowlist must update the test, forcing a conscious code review.

#include <QtTest/QtTest>

#include "util/PrivilegeEscalator.h"

#include <QProcessEnvironment>
#include <QSet>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

class TestPrivilegeEscalator : public QObject {
    Q_OBJECT

private slots:
    void allowlist_includesExpectedKeys();
    void allowlist_excludesDangerousKeys();
    void filterEnv_dropsLoaderKnobs();
    void filterEnv_keepsAllowlistedKeys();
    void filterEnv_skipsEmptyValues();
    void filterEnv_preservesValues();
    void filterEnv_dropsPathFromAllowlist();
    void allowlist_excludesQtPluginPath();
    void helperSearchPaths_isSafeFixedList();
    void findHelper_ignoresHostilePath();
};

void TestPrivilegeEscalator::allowlist_includesExpectedKeys()
{
    // The legitimate session-integration keys that allow the privileged
    // child to attach to the user's session. Removing any of these
    // would visibly break theming / display attachment / locale.
    const auto allow = util::PrivilegeEscalator::envAllowlist();
    for (const QByteArray &k : {
            QByteArray("DISPLAY"),
            QByteArray("WAYLAND_DISPLAY"),
            QByteArray("XAUTHORITY"),
            QByteArray("XDG_RUNTIME_DIR"),
            QByteArray("DBUS_SESSION_BUS_ADDRESS"),
            QByteArray("HOME"),
            QByteArray("LANG"),
            QByteArray("LC_ALL"),
        }) {
        QVERIFY2(allow.contains(k),
                 qPrintable(QStringLiteral("expected allowlist to contain %1")
                                .arg(QString::fromLatin1(k))));
    }
}

void TestPrivilegeEscalator::allowlist_excludesDangerousKeys()
{
    // ld.so / loader knobs — adding any of these to the allowlist is a
    // local-root vulnerability. Pinned here so a code change has to
    // touch this test deliberately.
    const auto allow = util::PrivilegeEscalator::envAllowlist();
    for (const QByteArray &k : {
            QByteArray("LD_PRELOAD"),
            QByteArray("LD_LIBRARY_PATH"),
            QByteArray("LD_AUDIT"),
            QByteArray("LD_DEBUG"),
            QByteArray("LD_BIND_NOW"),
            QByteArray("GCONV_PATH"),
            QByteArray("GETCONF_DIR"),
            QByteArray("HOSTALIASES"),
            QByteArray("LOCALDOMAIN"),
            QByteArray("LOCPATH"),
            QByteArray("MALLOC_TRACE"),
            QByteArray("NIS_PATH"),
            QByteArray("NLSPATH"),
            QByteArray("RESOLV_HOST_CONF"),
            QByteArray("RES_OPTIONS"),
            QByteArray("TMPDIR"),
            QByteArray("TZDIR"),
            // Toolkit plugin-path knobs that allow code injection via
            // a loaded plugin DLL.
            QByteArray("QT_PLUGIN_PATH"),
            QByteArray("QT_QPA_PLATFORM_PLUGIN_PATH"),
            QByteArray("GTK_MODULES"),
            QByteArray("GTK_PATH"),
            QByteArray("GIO_MODULE_DIR"),
            QByteArray("GIO_EXTRA_MODULES"),
            // Python / Perl / shell injection vectors a confused
            // helper script might honour.
            QByteArray("PYTHONPATH"),
            QByteArray("PYTHONHOME"),
            QByteArray("PERL5LIB"),
            QByteArray("PERL5OPT"),
            QByteArray("BASH_ENV"),
            QByteArray("ENV"),
            QByteArray("IFS"),
            QByteArray("CDPATH"),
        }) {
        QVERIFY2(!allow.contains(k),
                 qPrintable(QStringLiteral("DANGEROUS key %1 must NOT be on allowlist")
                                .arg(QString::fromLatin1(k))));
    }
}

void TestPrivilegeEscalator::filterEnv_dropsLoaderKnobs()
{
    QProcessEnvironment in;
    in.insert(QStringLiteral("LD_PRELOAD"),     QStringLiteral("/tmp/evil.so"));
    in.insert(QStringLiteral("LD_LIBRARY_PATH"), QStringLiteral("/tmp/evil"));
    in.insert(QStringLiteral("LD_AUDIT"),        QStringLiteral("/tmp/audit.so"));
    in.insert(QStringLiteral("QT_PLUGIN_PATH"),  QStringLiteral("/tmp/qtplugins"));
    in.insert(QStringLiteral("GTK_MODULES"),     QStringLiteral("evil-gtk-mod"));
    in.insert(QStringLiteral("HOME"),            QStringLiteral("/home/user"));

    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);

    QVERIFY(!out.contains(QStringLiteral("LD_PRELOAD")));
    QVERIFY(!out.contains(QStringLiteral("LD_LIBRARY_PATH")));
    QVERIFY(!out.contains(QStringLiteral("LD_AUDIT")));
    QVERIFY(!out.contains(QStringLiteral("QT_PLUGIN_PATH")));
    QVERIFY(!out.contains(QStringLiteral("GTK_MODULES")));
    QVERIFY(out.contains(QStringLiteral("HOME")));
}

void TestPrivilegeEscalator::filterEnv_keepsAllowlistedKeys()
{
    QProcessEnvironment in;
    in.insert(QStringLiteral("DISPLAY"),                  QStringLiteral(":0"));
    in.insert(QStringLiteral("WAYLAND_DISPLAY"),          QStringLiteral("wayland-0"));
    in.insert(QStringLiteral("XAUTHORITY"),               QStringLiteral("/run/user/1000/Xauth"));
    in.insert(QStringLiteral("XDG_RUNTIME_DIR"),          QStringLiteral("/run/user/1000"));
    in.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=/foo"));
    in.insert(QStringLiteral("LANG"),                     QStringLiteral("en_US.UTF-8"));

    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);

    QCOMPARE(out.value(QStringLiteral("DISPLAY")),       QStringLiteral(":0"));
    QCOMPARE(out.value(QStringLiteral("WAYLAND_DISPLAY")), QStringLiteral("wayland-0"));
    QCOMPARE(out.value(QStringLiteral("XAUTHORITY")),    QStringLiteral("/run/user/1000/Xauth"));
    QCOMPARE(out.value(QStringLiteral("XDG_RUNTIME_DIR")), QStringLiteral("/run/user/1000"));
    QCOMPARE(out.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")), QStringLiteral("unix:path=/foo"));
    QCOMPARE(out.value(QStringLiteral("LANG")),          QStringLiteral("en_US.UTF-8"));
}

void TestPrivilegeEscalator::filterEnv_dropsPathFromAllowlist()
{
    // PATH must NOT be forwarded by the allowlist filter — see comment in
    // PrivilegeEscalator.cpp. A user-controlled PATH in the privileged
    // child is a latent LPE primitive for any future relative-path exec.
    // sessionEnv() injects a safe absolute PATH separately.
    QProcessEnvironment in;
    in.insert(QStringLiteral("PATH"), QStringLiteral("/tmp/evil:/usr/bin"));
    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);
    QVERIFY(!out.contains(QStringLiteral("PATH")));
}

void TestPrivilegeEscalator::filterEnv_skipsEmptyValues()
{
    // An empty allowlisted value (e.g. `WAYLAND_DISPLAY=`) was being
    // forwarded as `WAYLAND_DISPLAY=` originally, which can confuse Qt
    // into a worse failure mode than not setting it at all. Current
    // behaviour: empty values are dropped.
    QProcessEnvironment in;
    in.insert(QStringLiteral("WAYLAND_DISPLAY"), QString());
    in.insert(QStringLiteral("DISPLAY"),         QStringLiteral(":0"));

    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);
    QVERIFY(!out.contains(QStringLiteral("WAYLAND_DISPLAY")));
    QVERIFY(out.contains(QStringLiteral("DISPLAY")));
}

void TestPrivilegeEscalator::filterEnv_preservesValues()
{
    // Values must round-trip byte-for-byte. A previous draft used
    // QString::arg("%1=%2") joining for an intermediate representation;
    // pin that values with '=' inside them survive.
    QProcessEnvironment in;
    in.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
              QStringLiteral("unix:path=/run/user/1000/bus,guid=abc=def"));

    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);
    QCOMPARE(out.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")),
             QStringLiteral("unix:path=/run/user/1000/bus,guid=abc=def"));
}

void TestPrivilegeEscalator::allowlist_excludesQtPluginPath()
{
    // Specifically pinned: QT_PLUGIN_PATH and QT_QPA_PLATFORM_PLUGIN_PATH
    // let an unprivileged attacker drop a .so into a writable directory
    // and have it loaded by the root child as a Qt plugin. Documented
    // in PrivilegeEscalator.cpp comments; codified here.
    const auto allow = util::PrivilegeEscalator::envAllowlist();
    QVERIFY(!allow.contains(QByteArray("QT_PLUGIN_PATH")));
    QVERIFY(!allow.contains(QByteArray("QT_QPA_PLATFORM_PLUGIN_PATH")));
}

void TestPrivilegeEscalator::helperSearchPaths_isSafeFixedList()
{
    // The helper-lookup directories must be exactly the safe absolute
    // dirs forced into the privileged child's PATH (see sessionEnv() /
    // scrubbedHelperEnv()). Anything user-writable or relative here
    // reopens the fake-pkexec phishing hole pinned below.
    const QStringList paths = util::PrivilegeEscalator::helperSearchPaths();
    QCOMPARE(paths, (QStringList{QStringLiteral("/usr/sbin"),
                                 QStringLiteral("/usr/bin"),
                                 QStringLiteral("/sbin"),
                                 QStringLiteral("/bin")}));
    for (const QString &p : paths)
        QVERIFY2(QDir::isAbsolutePath(p),
                 qPrintable(QStringLiteral("search path %1 must be absolute").arg(p)));
}

void TestPrivilegeEscalator::findHelper_ignoresHostilePath()
{
    // SECURITY (M12): findHelper must resolve helpers ONLY via the fixed
    // safe directory list, never via the user-controlled $PATH. A hostile
    // PATH containing a fake `pkexec` would otherwise have qiftop launch
    // the attacker's binary and prompt the user to authenticate to it.
    QTemporaryDir dir(QDir::currentPath()
                      + QStringLiteral("/hostile-path-XXXXXX"));
    QVERIFY(dir.isValid());

    // A helper name that cannot exist in the safe dirs, planted only in
    // the hostile directory.
    const QString fakeName = QStringLiteral("qiftop-test-fake-helper");
    const QString fakePath = dir.filePath(fakeName);
    {
        QFile f(fakePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\nexit 0\n");
    }
    QVERIFY(QFile::setPermissions(
        fakePath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    // Also plant a fake pkexec to simulate the real attack.
    const QString fakePkexec = dir.filePath(QStringLiteral("pkexec"));
    QVERIFY(QFile::copy(fakePath, fakePkexec));
    QVERIFY(QFile::setPermissions(
        fakePkexec,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", QStringLiteral("%1:%2")
                        .arg(dir.path(), QString::fromLocal8Bit(oldPath))
                        .toLocal8Bit());

    const QString foundFake   = util::PrivilegeEscalator::findHelper(fakeName);
    const QString foundPkexec = util::PrivilegeEscalator::findHelper(
        QStringLiteral("pkexec"));

    qputenv("PATH", oldPath);

    // The unique fake exists ONLY on the hostile PATH: must NOT be found.
    QVERIFY2(foundFake.isEmpty(),
             qPrintable(QStringLiteral("hostile-PATH helper was selected: %1")
                            .arg(foundFake)));
    // pkexec may or may not be installed in the safe dirs, but the fake
    // must never win, and any result must live in a safe directory.
    QVERIFY(foundPkexec != fakePkexec);
    if (!foundPkexec.isEmpty()) {
        bool inSafeDir = false;
        for (const QString &p : util::PrivilegeEscalator::helperSearchPaths())
            if (foundPkexec.startsWith(p + QLatin1Char('/'))) inSafeDir = true;
        QVERIFY2(inSafeDir,
                 qPrintable(QStringLiteral("pkexec resolved outside safe dirs: %1")
                                .arg(foundPkexec)));
    }
}

QTEST_APPLESS_MAIN(TestPrivilegeEscalator)
#include "test_priv_escalator.moc"
