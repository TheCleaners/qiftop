// qiftop-agent: privileged system-bus daemon that exposes the raw network
// statistics produced by the platform backend (libnl + libnetfilter_conntrack
// on Linux). Clients consume the data over DBus; they do not need
// CAP_NET_ADMIN themselves.
//
// Well-known name: org.qiftop.NetworkAgent1
//   Object: /org/qiftop/NetworkAgent1/Interfaces
//   Object: /org/qiftop/NetworkAgent1/Connections
//
// This translation unit is intentionally thin — argv parsing, choice of
// session vs. system bus, and concrete monitor construction only. The
// actual bus surface (service registration, name acquisition, IdleManager
// wiring) lives in agent::Application so it can be reused from in-process
// integration tests.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusError>

#include "Application.h"
#include "Config.h"
#include "backend/ProcessResolverFactory.h"
#include "dbus/Types.h"
#include "util/Logging.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ConntrackMonitor.h"
#include "backend/linux/NetlinkMonitor.h"
#endif

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
            .arg(QString::fromLatin1(qiftop::agent::kDefaultConfigPath)),
        QStringLiteral("path"),
        QString::fromLatin1(qiftop::agent::kDefaultConfigPath));
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

    const auto idleCfg = qiftop::agent::loadIdleConfig(parser.value(configOpt));
    auto resolver = qiftop::backend::createProcessResolver({});
    qiftop::agent::Application application(bus, &netMonitor, &connMonitor,
                                           idleCfg, std::move(resolver));
    if (!application.start()) {
        qCritical().noquote() << "agent:" << application.errorString();
        return 3;
    }
    return app.exec();
}
