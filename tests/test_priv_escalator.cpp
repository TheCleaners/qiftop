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

class TestPrivilegeEscalator : public QObject {
    Q_OBJECT

private slots:
    void allowlist_includesExpectedKeys();
    void allowlist_excludesDangerousKeys();
    void filterEnv_dropsLoaderKnobs();
    void filterEnv_keepsAllowlistedKeys();
    void filterEnv_skipsEmptyValues();
    void filterEnv_preservesValues();
    void allowlist_excludesQtPluginPath();
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
            QByteArray("PATH"),
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
    in.insert(QStringLiteral("PATH"),                     QStringLiteral("/usr/bin:/bin"));

    const QProcessEnvironment out = util::PrivilegeEscalator::filterEnv(in);

    QCOMPARE(out.value(QStringLiteral("DISPLAY")),       QStringLiteral(":0"));
    QCOMPARE(out.value(QStringLiteral("WAYLAND_DISPLAY")), QStringLiteral("wayland-0"));
    QCOMPARE(out.value(QStringLiteral("XAUTHORITY")),    QStringLiteral("/run/user/1000/Xauth"));
    QCOMPARE(out.value(QStringLiteral("XDG_RUNTIME_DIR")), QStringLiteral("/run/user/1000"));
    QCOMPARE(out.value(QStringLiteral("DBUS_SESSION_BUS_ADDRESS")), QStringLiteral("unix:path=/foo"));
    QCOMPARE(out.value(QStringLiteral("LANG")),          QStringLiteral("en_US.UTF-8"));
    QCOMPARE(out.value(QStringLiteral("PATH")),          QStringLiteral("/usr/bin:/bin"));
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

QTEST_APPLESS_MAIN(TestPrivilegeEscalator)
#include "test_priv_escalator.moc"
