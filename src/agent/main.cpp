// qiftop-agent: privileged system-bus daemon that exposes the raw network
// statistics produced by the platform backend (libnl + libnetfilter_conntrack
// on Linux). Clients consume the data over DBus; they do not need
// CAP_NET_ADMIN themselves.
//
// Well-known name: org.qiftop.NetworkAgent1
//   Object: /org/qiftop/NetworkAgent1/Interfaces
//   Object: /org/qiftop/NetworkAgent1/Connections

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusError>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QtDebug>

#include "ConnectionsService.h"
#include "IdleManager.h"
#include "InterfacesService.h"
#include "dbus/Types.h"
#include "util/Logging.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ConntrackMonitor.h"
#include "backend/linux/NetlinkMonitor.h"
#endif

namespace {

constexpr auto kBusName        = "org.qiftop.NetworkAgent1";
constexpr auto kIfacesPath     = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kConnsPath      = "/org/qiftop/NetworkAgent1/Connections";
constexpr auto kDefaultConfigPath = "/etc/qiftop/agent.conf";

// Clamp a numeric config value into [lo, hi]; warn on adjustment so admins
// notice typos in /etc/qiftop/agent.conf instead of silently getting
// degenerate cadences (or, worse, an unbounded hinted cadence that bypasses
// the documented absolute floor).
template <typename T>
static T clampCfg(const char *key, T raw, T lo, T hi, T fallback)
{
    if (raw < lo || raw > hi) {
        qWarning().noquote()
            << "agent: config key" << key << "value" << raw
            << "out of range [" << lo << "," << hi << "] — using" << fallback;
        return fallback;
    }
    return raw;
}

qiftop::agent::IdleManager::Config loadIdleConfig(const QString &path)
{
    qiftop::agent::IdleManager::Config cfg; // defaults
    if (!QFileInfo::exists(path)) {
        qCInfo(lcVerbose).noquote() << "agent: no config file at" << path
                                    << "— using built-in defaults";
        return cfg;
    }
    QSettings ini(path, QSettings::IniFormat);

    // Reasonable bounds: nothing below 10 ms (would be a hard DoS on the
    // netlink subsystem); nothing above one hour (clearly a typo).
    constexpr int kMinMs = 10;
    constexpr int kMaxMs = 60 * 60 * 1000;
    // Timeouts/windows can be zero (meaning "disable that step") or up to
    // ~24 hours; negative is always wrong.
    constexpr int kMinWin = 0;
    constexpr int kMaxWin = 24 * 60 * 60 * 1000;

    cfg.minIntervalMs    = clampCfg("poll/min_interval_ms",
                                    ini.value(QStringLiteral("poll/min_interval_ms"),
                                              cfg.minIntervalMs).toInt(),
                                    kMinMs, kMaxMs, cfg.minIntervalMs);
    cfg.activeIntervalMs = clampCfg("poll/base_interval_ms",
                                    ini.value(QStringLiteral("poll/base_interval_ms"),
                                              cfg.activeIntervalMs).toInt(),
                                    cfg.minIntervalMs, kMaxMs, cfg.activeIntervalMs);
    cfg.idleTimeoutMs    = clampCfg("idle/timeout_secs (ms)",
                                    ini.value(QStringLiteral("idle/timeout_secs"),
                                              cfg.idleTimeoutMs / 1000).toInt() * 1000,
                                    kMinWin, kMaxWin, cfg.idleTimeoutMs);
    cfg.hintTtlMs        = clampCfg("idle/hint_ttl_secs (ms)",
                                    ini.value(QStringLiteral("idle/hint_ttl_secs"),
                                              cfg.hintTtlMs / 1000).toInt() * 1000,
                                    kMinMs, kMaxWin, cfg.hintTtlMs);

    // schedule = active_window_secs:slow1_ms,slow1_window_secs:slow2_ms,slow2_window_secs:0
    // We accept the simpler form: three "<window_secs>:<interval_ms>" pairs.
    const QString sched = ini.value(QStringLiteral("idle/schedule"),
                                    QStringLiteral("30:2000,45:5000,60:0")).toString();
    const QStringList pairs = sched.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (pairs.size() >= 1) {
        const auto parts = pairs[0].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.activeWindowMs  = clampCfg("idle/schedule window1 (ms)",
                                           parts[0].trimmed().toInt() * 1000,
                                           kMinWin, kMaxWin, cfg.activeWindowMs);
            cfg.slow1IntervalMs = clampCfg("idle/schedule slow1 (ms)",
                                           parts[1].trimmed().toInt(),
                                           cfg.minIntervalMs, kMaxMs, cfg.slow1IntervalMs);
        }
    }
    if (pairs.size() >= 2) {
        const auto parts = pairs[1].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.slow1WindowMs   = clampCfg("idle/schedule window2 (ms)",
                                           parts[0].trimmed().toInt() * 1000,
                                           kMinWin, kMaxWin, cfg.slow1WindowMs);
            cfg.slow2IntervalMs = clampCfg("idle/schedule slow2 (ms)",
                                           parts[1].trimmed().toInt(),
                                           cfg.minIntervalMs, kMaxMs, cfg.slow2IntervalMs);
        }
    }
    if (pairs.size() >= 3) {
        const auto parts = pairs[2].split(QLatin1Char(':'));
        if (parts.size() == 2) {
            cfg.slow2WindowMs = clampCfg("idle/schedule window3 (ms)",
                                         parts[0].trimmed().toInt() * 1000,
                                         kMinWin, kMaxWin, cfg.slow2WindowMs);
            // third interval is the "paused" sentinel; we keep idleTimeoutMs separate
        }
    }
    qCInfo(lcVerbose).noquote()
        << "agent: loaded config" << path
        << "active=" << cfg.activeIntervalMs << "ms"
        << "schedule:" << cfg.activeWindowMs/1000 << "s→" << cfg.slow1IntervalMs << "ms,"
        << cfg.slow1WindowMs/1000  << "s→" << cfg.slow2IntervalMs << "ms,"
        << "idle=" << cfg.idleTimeoutMs/1000 << "s";
    return cfg;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("qiftop-agent"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "qiftop-agent — privileged DBus agent exposing network statistics."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption verboseOpt(QStringLiteral("verbose"),
        QStringLiteral("Enable verbose tracing on stderr."));
    QCommandLineOption sessionOpt(QStringLiteral("session"),
        QStringLiteral("Use the session bus instead of the system bus "
                       "(only for development)."));
    QCommandLineOption configOpt(QStringList{QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("Path to agent.conf (default: %1).")
            .arg(QString::fromLatin1(kDefaultConfigPath)),
        QStringLiteral("path"),
        QString::fromLatin1(kDefaultConfigPath));
    parser.addOption(verboseOpt);
    parser.addOption(sessionOpt);
    parser.addOption(configOpt);
    parser.process(app);

    util::logging::setVerbose(parser.isSet(verboseOpt));
    qiftop::dbus::registerTypes();

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();

    QDBusConnection bus = parser.isSet(sessionOpt)
        ? QDBusConnection::sessionBus()
        : QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        qCritical().noquote() << "agent: cannot connect to DBus:"
                              << bus.lastError().message();
        return 2;
    }

#ifdef BACKEND_LINUX
    NetlinkMonitor   netMonitor;
    ConntrackMonitor connMonitor;
#else
#   error "qiftop-agent currently requires a Linux backend"
#endif

    qiftop::agent::InterfacesService  ifaceSvc(&netMonitor);
    qiftop::agent::ConnectionsService connSvc(&connMonitor);

    // Register the service objects *before* requesting the bus name so that
    // a client triggered by DBus activation always finds them in place.
    constexpr auto kRegisterOpts = QDBusConnection::ExportAllContents;
    if (!bus.registerObject(QString::fromLatin1(kIfacesPath), &ifaceSvc, kRegisterOpts)) {
        qCritical() << "agent: failed to register Interfaces object:" << bus.lastError().message();
        return 3;
    }
    if (!bus.registerObject(QString::fromLatin1(kConnsPath), &connSvc, kRegisterOpts)) {
        qCritical() << "agent: failed to register Connections object:" << bus.lastError().message();
        return 3;
    }
    if (!bus.registerService(QString::fromLatin1(kBusName))) {
        qCritical().noquote() << "agent: failed to acquire bus name" << kBusName
                              << ":" << bus.lastError().message();
        return 4;
    }

    qCInfo(lcVerbose).noquote() << "agent: bus name acquired" << kBusName;

    const auto idleCfg = loadIdleConfig(parser.value(configOpt));
    qiftop::agent::IdleManager idle(&netMonitor, &connMonitor, idleCfg);
    idle.attachBus(bus); // drop hints immediately on peer disconnect
    ifaceSvc.setIdleManager(&idle);
    connSvc.setIdleManager(&idle);

    netMonitor.start();
    connMonitor.start();
    idle.noteActivity(); // ensure we start at the active interval

    return app.exec();
}
