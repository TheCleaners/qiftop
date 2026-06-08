#include "PrivilegeEscalator.h"
#include "Logging.h"

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>

namespace util {

namespace {

// Returns env vars to forward to the privileged child.
//
// SECURITY: This is an **allowlist**, not a denylist. The privileged child
// runs as root with the same (non-zero) ruid loader behaviour as any normal
// process — `AT_SECURE` is NOT set — so ld.so honours `LD_PRELOAD`,
// `LD_LIBRARY_PATH`, `LD_AUDIT`, `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`,
// `GTK_MODULES`, `GIO_MODULE_DIR`, etc., from whatever environment we hand it.
// A denylist will *always* lose this race against new ld.so / toolkit knobs;
// only an allowlist is safe.
//
// Anything not in this list is dropped. If a user-visible glitch turns out
// to be caused by a missing env var, add it here (and audit it for whether
// an attacker could weaponise it).
QStringList sessionEnv()
{
    static const QSet<QByteArray> kAllow = {
        // Display server / windowing
        "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY",
        "XDG_SESSION_TYPE", "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP",
        // Session DBus (per-user; root child uses it for tray / notifications)
        "DBUS_SESSION_BUS_ADDRESS",
        // Locale
        "LANG", "LANGUAGE",
        "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
        "LC_MONETARY", "LC_MESSAGES", "LC_PAPER", "LC_NAME", "LC_ADDRESS",
        "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION",
        // Working directory + PATH (PATH is needed so the child can find its
        // own helpers; pkexec sets a safe default if we omit it).
        "HOME", "PWD", "PATH",
        // Theme integration. These are read by Qt directly; QT_PLUGIN_PATH /
        // QT_QPA_PLATFORM_PLUGIN_PATH are deliberately NOT in this list
        // because they let an attacker inject a Qt plugin into the root
        // process. Qt's built-in defaults find the system plugins fine.
        "QT_STYLE_OVERRIDE", "QT_QPA_PLATFORMTHEME", "QT_SCALE_FACTOR",
        "QT_AUTO_SCREEN_SCALE_FACTOR",
    };

    QStringList out;
    const auto env = QProcessEnvironment::systemEnvironment();
    const auto keys = env.keys();
    for (const QString &k : keys) {
        if (!kAllow.contains(k.toLocal8Bit())) continue;
        const QString v = env.value(k);
        if (v.isEmpty()) continue;
        out << QStringLiteral("%1=%2").arg(k, v);
    }
    // The handoff socket path and nonce are added explicitly. They're
    // host-local to the user (abstract or under $XDG_RUNTIME_DIR); the
    // protocol authenticates the connection via the nonce (see HandoffServer).
    const QString handoff = qEnvironmentVariable("QIFTOP_HANDOFF_SOCKET");
    if (!handoff.isEmpty())
        out << QStringLiteral("QIFTOP_HANDOFF_SOCKET=%1").arg(handoff);
    const QString handoffNonce = qEnvironmentVariable("QIFTOP_HANDOFF_NONCE");
    if (!handoffNonce.isEmpty())
        out << QStringLiteral("QIFTOP_HANDOFF_NONCE=%1").arg(handoffNonce);
    return out;
}

// Build a QProcessEnvironment containing only the allowlisted variables.
// Used to scrub the env of helpers (kdesu, gksudo, lxqt-sudo, beesu,
// x-terminal-emulator+sudo) that inherit the parent's QProcess env by
// default — sudo's `env_reset` saves us in some cases but several of these
// helpers don't go through sudo, and even sudo's defaults can be relaxed
// site-locally.
QProcessEnvironment scrubbedHelperEnv()
{
    QProcessEnvironment out;
    const auto src = QProcessEnvironment::systemEnvironment();
    const QStringList lines = sessionEnv();
    for (const QString &line : lines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        out.insert(line.left(eq), line.mid(eq + 1));
    }
    Q_UNUSED(src);
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
    // SECURITY: scrub the helper's own env. Several helpers (kdesu, gksudo,
    // lxqt-sudo, beesu, x-terminal-emulator) inherit our environment by
    // default and forward it to the privileged child. Strip everything not
    // on the allowlist; pkexec re-builds via `env` argv (see runPkexec) and
    // therefore depends only on sessionEnv() — which is also allowlist-only.
    p->setProcessEnvironment(scrubbedHelperEnv());
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
