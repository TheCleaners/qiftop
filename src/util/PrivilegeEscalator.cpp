#include "PrivilegeEscalator.h"
#include "Logging.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>

namespace util {

namespace {

// Returns env vars to forward to the privileged child. We pass *almost* the
// full environment so the elevated GUI inherits the user's theme, locale,
// session bus, XDG paths, Qt/KDE/GTK settings, etc. A short denylist drops
// variables that either don't make sense for a different uid (auth sockets,
// the launcher's own bookkeeping) or would actively break (e.g. dbus user
// sessions tied to the original uid).
QStringList sessionEnv()
{
    static const QSet<QByteArray> kDeny = {
        // Auth/agent sockets tied to the calling uid
        "SSH_AUTH_SOCK", "SSH_AGENT_PID", "GPG_AGENT_INFO",
        // Helper bookkeeping that will be reset or is inappropriate as root
        "SUDO_USER", "SUDO_UID", "SUDO_GID", "SUDO_COMMAND",
        "PKEXEC_UID",
        "_", "OLDPWD",
        // Set fresh by pkexec / login
        "USER", "USERNAME", "LOGNAME", "MAIL", "SHELL",
        // Our own one-shot; cleared by the parent before launch anyway
        "QIFTOP_HANDOFF_SOCKET",
    };

    QStringList out;
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto keys = env.keys();
    for (const QString &k : keys) {
        if (kDeny.contains(k.toLocal8Bit())) continue;
        const QString v = env.value(k);
        if (v.isEmpty()) continue;
        out << QStringLiteral("%1=%2").arg(k, v);
    }
    // QIFTOP_HANDOFF_SOCKET is added explicitly so the parent can still hand
    // it to the child even though it's on the deny list above (the deny list
    // exists so the privileged child doesn't recurse).
    const QString handoff = qEnvironmentVariable("QIFTOP_HANDOFF_SOCKET");
    if (!handoff.isEmpty())
        out << QStringLiteral("QIFTOP_HANDOFF_SOCKET=%1").arg(handoff);
    return out;
}

QString findHelper(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

// Start a helper as a regular QProcess (NOT detached). The QProcess is
// parented to QCoreApplication so it outlives the (possibly stack-local)
// escalator object — that's what keeps pkexec / polkit happy: they verify
// their caller (this app) is still alive when presenting the auth dialog.
// The caller is responsible for keeping the app running until the user
// dismisses the old instance (or for our IPC handoff to trigger quit).
bool launchAttached(const QString &program,
                    const QStringList &args,
                    bool verbose)
{
    auto *p = new QProcess(QCoreApplication::instance());
    p->setProcessChannelMode(QProcess::ForwardedChannels);
    if (verbose)
        qCInfo(lcVerbose).noquote() << "exec:" << program << args;
    p->start(program, args);
    if (!p->waitForStarted(3000)) {
        if (verbose)
            qCWarning(lcVerbose).noquote() << "  failed to start:" << p->errorString();
        p->deleteLater();
        return false;
    }
    QObject::connect(p, &QProcess::finished, p, [p, verbose](int code, QProcess::ExitStatus st) {
        if (verbose)
            qCInfo(lcVerbose) << "helper exited code=" << code << "status=" << int(st);
        p->deleteLater();
    });
    return true;
}

} // namespace

PrivilegeEscalator::PrivilegeEscalator(QObject *parent)
    : QObject(parent)
{}

QStringList PrivilegeEscalator::desktopTokens() const
{
    // XDG_CURRENT_DESKTOP can be colon-separated (e.g. "ubuntu:GNOME").
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP")
        .split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

DesktopEnv PrivilegeEscalator::detectDesktop() const
{
    const QStringList tokens = desktopTokens();
    for (const QString &raw : tokens) {
        const QString t = raw.toUpper();
        if (t.contains(QLatin1String("KDE")))      return DesktopEnv::KDE;
        if (t.contains(QLatin1String("GNOME")))    return DesktopEnv::GNOME;
        if (t.contains(QLatin1String("XFCE")))     return DesktopEnv::XFCE;
        if (t.contains(QLatin1String("MATE")))     return DesktopEnv::MATE;
        if (t.contains(QLatin1String("CINNAMON"))) return DesktopEnv::Cinnamon;
        if (t.contains(QLatin1String("LXQT")))     return DesktopEnv::LXQt;
    }
    if (!tokens.isEmpty()) return DesktopEnv::Other;
    return DesktopEnv::Unknown;
}

QList<PrivilegeEscalator::Strategy> PrivilegeEscalator::orderedStrategies() const
{
    // All strategies we know about; one entry per helper. Order within this
    // initialiser is the "natural" preference; orderedStrategies() then
    // re-sorts so the DE-preferred helpers come first.
    const Strategy pkexec  {"pkexec",       [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runPkexec(p, a); }};
    const Strategy kdesu   {"kdesu",        [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runKdesu(p, a); }};
    const Strategy gksu    {"gksudo",       [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runGksu("gksudo", p, a); }};
    const Strategy lxqtSu  {"lxqt-sudo",    [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runGksu("lxqt-sudo", p, a); }};
    const Strategy beesu   {"beesu",        [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runGksu("beesu", p, a); }};
    const Strategy termSu  {"x-terminal+sudo", [](PrivilegeEscalator *e, const QString &p, const QStringList &a){ return e->runTermSudo(p, a); }};

    QList<Strategy> ordered;
    switch (detectDesktop()) {
    case DesktopEnv::KDE:
        ordered = {kdesu, pkexec, gksu, lxqtSu, beesu, termSu};
        break;
    case DesktopEnv::LXQt:
        ordered = {lxqtSu, pkexec, kdesu, gksu, beesu, termSu};
        break;
    case DesktopEnv::GNOME:
    case DesktopEnv::Cinnamon:
    case DesktopEnv::MATE:
    case DesktopEnv::XFCE:
    case DesktopEnv::Other:
    case DesktopEnv::Unknown:
    default:
        ordered = {pkexec, gksu, kdesu, lxqtSu, beesu, termSu};
        break;
    }
    return ordered;
}

QStringList PrivilegeEscalator::plannedStrategies() const
{
    QStringList out;
    for (const Strategy &s : orderedStrategies())
        out << s.id;
    return out;
}

bool PrivilegeEscalator::relaunch(const QString &program,
                                  const QStringList &args,
                                  QString *usedStrategy)
{
    if (m_verbose) {
        qCInfo(lcVerbose).noquote() << "relaunch: program=" << program
                                    << " args=" << args
                                    << " desktop=" << desktopTokens();
    }
    for (const Strategy &s : orderedStrategies()) {
        emit status(tr("Trying %1…").arg(s.id));
        if (m_verbose)
            qCInfo(lcVerbose).noquote() << "trying strategy:" << s.id;
        if (s.run(this, program, args)) {
            if (usedStrategy) *usedStrategy = s.id;
            emit status(tr("Launched via %1.").arg(s.id));
            return true;
        }
    }
    emit status(tr("No privilege helper succeeded."));
    return false;
}

// ----- individual helpers ---------------------------------------------------

bool PrivilegeEscalator::runPkexec(const QString &program, const QStringList &args)
{
    const QString path = findHelper(QStringLiteral("pkexec"));
    if (path.isEmpty()) return false;

    // pkexec sanitises the environment of the child. To get the GUI to attach
    // to the user's session we re-inject the variables via `env` *after*
    // pkexec, which runs them as root via the standard PATH lookup.
    QStringList cmd;
    cmd << QStringLiteral("env") << sessionEnv() << program << args;
    return launchAttached(path, cmd, m_verbose);
}

bool PrivilegeEscalator::runKdesu(const QString &program, const QStringList &args)
{
    // kdesu (KDE 5/6 binary is sometimes `kdesu`, sometimes `kdesudo`).
    QString path = findHelper(QStringLiteral("kdesu"));
    if (path.isEmpty()) path = findHelper(QStringLiteral("kdesudo"));
    if (path.isEmpty()) return false;

    QStringList cmd;
    cmd << QStringLiteral("-c");
    // kdesu's -c expects a single shell-quoted command string.
    QStringList parts; parts << program << args;
    for (QString &p : parts) p = QStringLiteral("'%1'").arg(p.replace(QLatin1Char('\''), QLatin1String("'\\''")));
    cmd << parts.join(QLatin1Char(' '));
    return launchAttached(path, cmd, m_verbose);
}

bool PrivilegeEscalator::runGksu(const QString &id, const QString &program, const QStringList &args)
{
    const QString path = findHelper(id);
    if (path.isEmpty()) return false;

    QStringList cmd;
    cmd << program << args;
    return launchAttached(path, cmd, m_verbose);
}

bool PrivilegeEscalator::runTermSudo(const QString &program, const QStringList &args)
{
    // Last-resort fallback: open a terminal so that the user has somewhere to
    // type their password. Not pretty, but at least it works on any minimal
    // X11/Wayland session that lacks polkit / kdesu / gksudo.
    QString term = findHelper(QStringLiteral("x-terminal-emulator"));
    if (term.isEmpty()) term = findHelper(QStringLiteral("xterm"));
    if (term.isEmpty()) return false;

    const QString sudo = findHelper(QStringLiteral("sudo"));
    if (sudo.isEmpty()) return false;

    QStringList cmd;
    cmd << QStringLiteral("-e") << sudo << program << args;
    return launchAttached(term, cmd, m_verbose);
}

} // namespace util
