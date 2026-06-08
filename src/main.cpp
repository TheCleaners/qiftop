#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QIcon>
#include <QTimer>

#include "config/Settings.h"
#include "dbus/Types.h"
#include "dns/QtDnsResolver.h"
#include "ui/MainWindow.h"
#include "util/HandoffClient.h"
#include "util/Logging.h"

#include "backend/dbus/DBusConnectionMonitor.h"
#include "backend/dbus/DBusNetworkMonitor.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ConntrackMonitor.h"
#include "backend/linux/NetlinkMonitor.h"
#endif

namespace {

constexpr auto kAgentBusName = "org.qiftop.NetworkAgent1";
constexpr auto kAgentIfacesPath = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kAgentIfacesIface = "org.qiftop.NetworkAgent1.Interfaces";

struct AgentProbe {
    bool        reachable = false;
    QString     version;       // e.g. "0.1"; empty if pre-Version agents
    QStringList capabilities;  // token list; empty if unsupported
};

// Try to fetch a property via the standard freedesktop Properties interface.
// Returns a default-constructed QVariant when the call fails — older agents
// don't expose Version/Capabilities, and we'd rather use a sane default
// ("legacy agent") than refuse to use them.
QVariant propGet(QDBusConnection &bus,
                 const QString   &path,
                 const QString   &iface,
                 const QString   &name)
{
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kAgentBusName),
                                               path,
                                               QStringLiteral("org.freedesktop.DBus.Properties"),
                                               QStringLiteral("Get"));
    call << iface << name;
    auto reply = bus.call(call, QDBus::Block, 1000);
    if (reply.type() == QDBusMessage::ErrorMessage) return {};
    const auto args = reply.arguments();
    if (args.isEmpty()) return {};
    return args.first().value<QDBusVariant>().variant();
}

// Probes the privileged agent on the system bus AND verifies our DBus policy
// permits us to call its methods. We do BOTH a name lookup (to trigger DBus
// activation if needed) and a low-cost probe call against the Interfaces
// service, because the bus policy is now group-gated (`netdev`) and a
// non-netdev user would see name registration succeed but every subsequent
// method call return AccessDenied — better to fall back to the in-process
// backend cleanly than to leave the UI showing an empty table forever.
//
// Also opportunistically reads the Version and Capabilities properties so
// the UI can show "qiftop-agent vX.Y" in the status bar and gate optional
// feature use on token presence. Both are empty for pre-property agents.
AgentProbe probeAgent()
{
    AgentProbe info;
    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) return info;
    auto *iface = bus.interface();
    if (!iface) return info;
    if (!iface->isServiceRegistered(QString::fromLatin1(kAgentBusName))) {
        auto reply = iface->startService(QString::fromLatin1(kAgentBusName));
        if (!reply.isValid()) return info;
    }
    QDBusMessage probe = QDBusMessage::createMethodCall(
        QString::fromLatin1(kAgentBusName),
        QString::fromLatin1(kAgentIfacesPath),
        QString::fromLatin1(kAgentIfacesIface),
        QStringLiteral("GetInterfaces"));
    QDBusMessage reply = bus.call(probe, QDBus::Block, 1000);
    if (reply.type() == QDBusMessage::ErrorMessage) return info;
    info.reachable = true;

    const QVariant v = propGet(bus,
                               QString::fromLatin1(kAgentIfacesPath),
                               QString::fromLatin1(kAgentIfacesIface),
                               QStringLiteral("Version"));
    if (v.isValid()) info.version = v.toString();
    const QVariant c = propGet(bus,
                               QString::fromLatin1(kAgentIfacesPath),
                               QString::fromLatin1(kAgentIfacesIface),
                               QStringLiteral("Capabilities"));
    if (c.isValid()) info.capabilities = c.toStringList();
    return info;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("qiftop"));
    QCoreApplication::setApplicationName(QStringLiteral("qiftop"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1"));
    // Picks up dist/desktop/qiftop.svg from the hicolor theme once installed;
    // falls through silently when running from the build tree.
    QGuiApplication::setDesktopFileName(QStringLiteral("qiftop"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("qiftop")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("qiftop — Qt6 network monitor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption verboseOpt(QStringLiteral("verbose"),
        QStringLiteral("Print diagnostic information to stderr "
                       "(privilege escalation, backend errors, …)."));
    QCommandLineOption noAgentOpt(QStringLiteral("no-agent"),
        QStringLiteral("Skip the system-bus agent and always use the in-process "
                       "backend (development / debugging)."));
    QCommandLineOption ifaceOpt(QStringList{QStringLiteral("i"), QStringLiteral("interface")},
        QStringLiteral("Open on the Connections tab and restrict it to flows on "
                       "<iface> (may be given more than once). Matches iftop(8)."),
        QStringLiteral("iface"));
    QCommandLineOption trayOpt(QStringLiteral("tray"),
        QStringLiteral("Start with no main window visible — only the system tray "
                       "icon. Used by the XDG autostart entry installed via "
                       "Settings → Tray → Start on login."));
    parser.addOption(verboseOpt);
    parser.addOption(noAgentOpt);
    parser.addOption(ifaceOpt);
    parser.addOption(trayOpt);
    parser.process(app);

    util::logging::setVerbose(parser.isSet(verboseOpt));

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qiftop::dbus::registerTypes();

    Settings      settings;
    QtDnsResolver dns;

    // Pick the data source. Prefer the privileged DBus agent so the UI never
    // needs CAP_NET_ADMIN of its own; fall back to in-process backends (which
    // will surface a "Relaunch as administrator" banner if they hit EPERM).
    std::unique_ptr<NetworkMonitor>    netMonitor;
    std::unique_ptr<ConnectionMonitor> connMonitor;

    const auto probe = parser.isSet(noAgentOpt) ? AgentProbe{} : probeAgent();
    const bool useAgent = probe.reachable;
    if (useAgent) {
        qCInfo(lcVerbose) << "main: using DBus agent" << kAgentBusName
                          << "version=" << probe.version
                          << "caps=" << probe.capabilities;
        netMonitor  = std::make_unique<qiftop::backend::dbus_client::DBusNetworkMonitor>();
        connMonitor = std::make_unique<qiftop::backend::dbus_client::DBusConnectionMonitor>();
    } else {
#ifdef BACKEND_LINUX
        qCInfo(lcVerbose) << "main: DBus agent unavailable, using in-process backend";
        netMonitor  = std::make_unique<NetlinkMonitor>();
        connMonitor = std::make_unique<ConntrackMonitor>();
#else
#       error "No backend available"
#endif
    }

    MainWindow window(&settings, netMonitor.get(), connMonitor.get(), &dns);
    window.setBackendInfo(useAgent, probe.version, probe.capabilities);
    // If we're on the DBus path, forward the agent's CadenceChanged signal
    // into the UI so the status bar can tint when the agent slows/pauses.
    if (useAgent) {
        if (auto *dbusNet = qobject_cast<qiftop::backend::dbus_client::DBusNetworkMonitor*>(
                netMonitor.get())) {
            QObject::connect(dbusNet, &qiftop::backend::dbus_client::DBusNetworkMonitor::agentCadenceChanged,
                             &window, &MainWindow::notifyAgentCadence);
        }
    }
    // --tray suppresses the initial window show; the tray icon (set up
    // by MainWindow's ctor) remains the only visible surface, and the
    // user can click it to summon the window. If the tray turns out
    // not to be available (no host), the window will be unreachable
    // until they re-launch without --tray — we log a warning then.
    const bool startInTray = parser.isSet(trayOpt);
    if (!startInTray)
        window.show();
    else
        qCInfo(lcVerbose) << "main: --tray set; not showing main window at startup";

    // -i <iface> [-i <iface> …] — restrict the Connections view to the named
    // interfaces (transient, not persisted) and jump to that tab.
    const QStringList cliIfaces = parser.values(ifaceOpt);
    if (!cliIfaces.isEmpty()) {
        QStringList unique = cliIfaces;
        unique.removeDuplicates();
        settings.setConnectionVisibleIfacesTransient(unique);
        window.selectConnectionsTab();
    }

    netMonitor->start();
    connMonitor->start();

    // If launched by an unprivileged sibling that's waiting for us to come up,
    // open the persistent IPC channel, authenticate with the nonce the parent
    // baked into our env, send READY, and keep it alive so the sibling can act
    // as our tray host (it sees stats; we obey SHOW/PAUSE/QUIT).
    util::HandoffClient handoffClient;
    const QByteArray handoffPath = qgetenv("QIFTOP_HANDOFF_SOCKET");
    if (!handoffPath.isEmpty()) {
        qunsetenv("QIFTOP_HANDOFF_SOCKET"); // don't recurse into our own children
        // Clear the nonce from env AFTER HandoffClient::connectTo() reads it,
        // so it doesn't leak to any QProcess we later spawn.
        if (handoffClient.connectTo(QString::fromLocal8Bit(handoffPath))) {
            window.attachHandoffClient(&handoffClient);
            QTimer::singleShot(0, &handoffClient, &util::HandoffClient::sendReady);
        }
        qunsetenv("QIFTOP_HANDOFF_NONCE");
    }

    return app.exec();
}
