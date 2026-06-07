#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace qiftop::autostart {

namespace {

constexpr auto kFileName = "qiftop.desktop";

QString autostartDir()
{
    // QStandardPaths::GenericConfigLocation = $XDG_CONFIG_HOME or
    // ~/.config — autostart/ underneath is the XDG-spec'd path that
    // every desktop honors (GNOME, KDE, Xfce, LXQt, MATE, Cinnamon).
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QLatin1String("/autostart");
}

// Locate the installed qiftop binary. Prefer the absolute path of the
// currently-running process (works even when launched from build dir
// during development), with a sensible fallback to /usr/bin/qiftop for
// the packaged case.
QString execPath()
{
    const QString applied = QCoreApplication::applicationFilePath();
    if (!applied.isEmpty() && QFileInfo(applied).isExecutable())
        return applied;
    return QStringLiteral("/usr/bin/qiftop");
}

} // namespace

QString entryPath()
{
    return autostartDir() + QLatin1Char('/') + QLatin1String(kFileName);
}

bool isEnabled()
{
    const QString path = entryPath();
    if (!QFile::exists(path))
        return false;
    // .desktop files are INI-format; the Desktop Entry Specification
    // says Hidden=true means "ignore this entry" and GNOME also
    // observes X-GNOME-Autostart-enabled=false.
    QSettings ini(path, QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("Desktop Entry"));
    if (ini.value(QStringLiteral("Hidden"), false).toBool())
        return false;
    if (!ini.value(QStringLiteral("X-GNOME-Autostart-enabled"), true).toBool())
        return false;
    return true;
}

bool setEnabled(bool enabled)
{
    const QString path = entryPath();
    if (!enabled) {
        if (!QFile::exists(path))
            return true;
        QFile f(path);
        if (!f.remove()) {
            qWarning("autostart: failed to remove %s: %s",
                     qUtf8Printable(path), qUtf8Printable(f.errorString()));
            return false;
        }
        return true;
    }

    if (!QDir().mkpath(autostartDir())) {
        qWarning("autostart: failed to create %s", qUtf8Printable(autostartDir()));
        return false;
    }

    // Hand-rolled because it's 8 keys and that's smaller than parsing
    // the installed dist/desktop/qiftop.desktop, mutating Exec, and
    // re-serializing. Keys are stable XDG vocabulary.
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning("autostart: failed to open %s: %s",
                 qUtf8Printable(path), qUtf8Printable(f.errorString()));
        return false;
    }
    const QString body = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=qiftop\n"
        "Comment=Qt6 iftop-style network monitor\n"
        "Exec=%1 --tray\n"
        "Icon=qiftop\n"
        "Terminal=false\n"
        "Categories=Network;Monitor;\n"
        "X-GNOME-Autostart-enabled=true\n"
        "Hidden=false\n")
        .arg(execPath());
    f.write(body.toUtf8());
    f.close();
    // .desktop files in autostart don't need +x — XDG launchers exec
    // the Exec= line directly. Permissions left at the default (umask).
    return true;
}

} // namespace qiftop::autostart
