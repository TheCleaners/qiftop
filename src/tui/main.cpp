// nqiftop — ncurses frontend for qiftop, built entirely on libqiftop.
//
// Data source mirrors the GUI: probe the privileged DBus agent, else fall
// back to the in-process capture backend (needs root). The aggregators,
// formatting, and DBus client all come from libqiftop — no Qt Widgets, so
// this runs over SSH on a headless box.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QHostAddress>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <csignal>
#include <cerrno>
#include <memory>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/ConnectionMonitor.h"
#include "backend/MonitorCapabilities.h"
#include "backend/NetworkMonitor.h"
#include "backend/PlatformInfo.h"
#include "backend/dbus/DBusConnectionMonitor.h"
#include "backend/dbus/DBusNetworkMonitor.h"
#include "dbus/Types.h"
#include "dns/QtDnsResolver.h"
#include "tui/Screen.h"
#include "tui/TuiApp.h"
#include "util/Logging.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ConntrackMonitor.h"
#include "backend/linux/NetlinkMonitor.h"
#endif

#ifdef BACKEND_BSD
#include "backend/bsd/BsdConnectionMonitor.h"
#include "backend/bsd/BsdNetworkMonitor.h"
#endif

// ncurses last (ERR, resizeterm) — after Qt to avoid macro clashes.
#include "tui/Curses.h"

#ifndef QIFTOP_VERSION
#define QIFTOP_VERSION "0.0-dev"
#endif

using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;
using qiftop::backend::dbus_client::DBusNetworkMonitor;

namespace {

constexpr auto kAgentBusName    = "org.qiftop.NetworkAgent1";
constexpr auto kAgentIfacesPath = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kAgentIfacesIface = "org.qiftop.NetworkAgent1.Interfaces";

// Mirrors the GUI's AgentProbe (src/main.cpp): reachability plus the
// opportunistically-read Version / Capabilities so the TUI can gate the
// optional Process/Container columns on the agent's wire tokens.
struct AgentProbe {
    bool        reachable = false;
    QString     version;
    QStringList capabilities;
};

// Fetch a property via the freedesktop Properties interface; a default QVariant
// when the call fails (pre-property agents) — caller falls back to a sane default.
QVariant propGet(QDBusConnection &bus, const QString &path,
                 const QString &iface, const QString &name)
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

// Minimal liveness probe: name-activate the agent and do a real method call
// (the bus policy is group-gated, so registration alone isn't enough). Then
// opportunistically read Version / Capabilities like the GUI.
AgentProbe probeAgent(bool sessionBus)
{
    AgentProbe info;
    auto bus = sessionBus ? QDBusConnection::sessionBus() : QDBusConnection::systemBus();
    if (!bus.isConnected()) return info;
    auto *iface = bus.interface();
    if (!iface) return info;
    if (!iface->isServiceRegistered(QString::fromLatin1(kAgentBusName))) {
        if (!iface->startService(QString::fromLatin1(kAgentBusName)).isValid())
            return info;
    }
    QDBusMessage probe = QDBusMessage::createMethodCall(
        QString::fromLatin1(kAgentBusName),
        QString::fromLatin1(kAgentIfacesPath),
        QString::fromLatin1(kAgentIfacesIface),
        QStringLiteral("GetInterfaces"));
    if (bus.call(probe, QDBus::Block, 1000).type() == QDBusMessage::ErrorMessage)
        return info;
    info.reachable = true;

    const QVariant v = propGet(bus, QString::fromLatin1(kAgentIfacesPath),
                               QString::fromLatin1(kAgentIfacesIface),
                               QStringLiteral("Version"));
    if (v.isValid()) info.version = v.toString();
    const QVariant c = propGet(bus, QString::fromLatin1(kAgentIfacesPath),
                               QString::fromLatin1(kAgentIfacesIface),
                               QStringLiteral("Capabilities"));
    if (c.isValid()) info.capabilities = c.toStringList();
    return info;
}

// Self-pipe for async-signal-safe handling: the handler only write()s the
// signal number; a QSocketNotifier processes it on the event loop.
int g_sigPipe[2] = {-1, -1};
void signalHandler(int sig)
{
    const unsigned char b = static_cast<unsigned char>(sig);
    ::ssize_t n = ::write(g_sigPipe[1], &b, 1);
    (void)n;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("qiftop"));
    QCoreApplication::setApplicationName(QStringLiteral("nqiftop"));
    QCoreApplication::setApplicationVersion(QStringLiteral(QIFTOP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("nqiftop — ncurses network monitor (qiftop)"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption sessionOpt(QStringLiteral("session"),
        QStringLiteral("Talk to the agent on the SESSION bus (development)."));
    QCommandLineOption noAgentOpt(QStringLiteral("no-agent"),
        QStringLiteral("Skip the agent and capture in-process (needs root)."));
    QCommandLineOption verboseOpt(QStringLiteral("verbose"),
        QStringLiteral("Diagnostics to stderr."));
    QCommandLineOption intervalOpt(QStringList{QStringLiteral("i"), QStringLiteral("interval")},
        QStringLiteral("Poll interval in milliseconds (default 1000)."),
        QStringLiteral("ms"), QStringLiteral("1000"));
    QCommandLineOption themeOpt(QStringLiteral("theme"),
        QStringLiteral("Colour theme: %1 (default dark; 'z' cycles live).")
            .arg(qiftop::tui::themeNames().join(QStringLiteral(", "))),
        QStringLiteral("name"), QStringLiteral("dark"));
    QCommandLineOption viewOpt(QStringLiteral("view"),
        QStringLiteral("Initial tab: interfaces | connections (default: last used)."),
        QStringLiteral("tab"));
    QCommandLineOption groupOpt(QStringLiteral("group"),
        QStringLiteral("Group connections: off | interface | process | container "
                       "(default: last used)."),
        QStringLiteral("mode"));
    parser.addOption(sessionOpt);
    parser.addOption(noAgentOpt);
    parser.addOption(verboseOpt);
    parser.addOption(intervalOpt);
    parser.addOption(themeOpt);
    parser.addOption(viewOpt);
    parser.addOption(groupOpt);
    parser.process(app);

    util::logging::setVerbose(parser.isSet(verboseOpt));
    bool ok = false;
    int pollMs = parser.value(intervalOpt).toInt(&ok);
    if (!ok || pollMs < 100) pollMs = 1000;
    const bool sessionBus = parser.isSet(sessionOpt);

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qiftop::dbus::registerTypes();

    // --- source selection ---
    std::unique_ptr<NetworkMonitor>    netMon;
    std::unique_ptr<ConnectionMonitor> connMon;
    QString sourceLabel;
    const AgentProbe agent = parser.isSet(noAgentOpt) ? AgentProbe{} : probeAgent(sessionBus);
    const bool useAgent = agent.reachable;
    if (useAgent) {
        auto net = std::make_unique<DBusNetworkMonitor>(sessionBus);
        // Agent publishes one merged Capabilities list; let it ride on the
        // network proxy (probeAgent already fetched it). The connection proxy
        // reports empty; the union below recombines them.
        net->setAgentCapabilities(agent.capabilities);
        netMon  = std::move(net);
        connMon = std::make_unique<DBusConnectionMonitor>(sessionBus);
        sourceLabel = sessionBus ? QStringLiteral("agent (session)")
                                 : QStringLiteral("agent");
    } else {
#ifdef BACKEND_LINUX
        netMon  = std::make_unique<NetlinkMonitor>();
        connMon = std::make_unique<ConntrackMonitor>();
        sourceLabel = QStringLiteral("in-process");
#elif defined(BACKEND_BSD)
        netMon  = std::make_unique<qiftop::backend::bsd::BsdNetworkMonitor>();
        connMon = std::make_unique<qiftop::backend::bsd::BsdConnectionMonitor>();
        sourceLabel = QStringLiteral("in-process");
#else
        std::fprintf(stderr, "nqiftop: no capture backend on this platform\n");
        return 1;
#endif
    }

    // --- data pipeline (libqiftop aggregators) ---
    InterfaceAggregator  ifaceAgg;
    ConnectionAggregator connAgg;
    connAgg.setRateSmoothingMs(300);
    connAgg.setPollIntervalMs(pollMs);

    // Reverse-DNS resolver (client-side, async, cached). Wired but disabled
    // until the user enables "Resolve hostnames" in the settings modal.
    QtDnsResolver dnsResolver;
    connAgg.setDnsResolver(&dnsResolver);

    QObject::connect(netMon.get(),  &NetworkMonitor::statsUpdated,
                     &ifaceAgg, &InterfaceAggregator::updateStats);
    QObject::connect(connMon.get(), &ConnectionMonitor::connectionsUpdated,
                     &connAgg, &ConnectionAggregator::updateConnections);

    // Keep the connection aggregator's notion of "our own addresses" current,
    // so direction inference + forwarded-flow detection (and therefore the
    // colour coding) work. Seed immediately from the platform, then refresh
    // from the live interface stats (CIDRs) as they change — like the GUI.
    connAgg.setLocalAddresses(qiftop::platform::localAddresses());
    const auto refreshLocalAddrs = [&ifaceAgg, &connAgg] {
        QSet<QHostAddress> locals;
        for (const auto &r : ifaceAgg.rows()) {
            for (const QString &cidr : r.current.addresses) {
                const QHostAddress a(cidr.left(cidr.indexOf(QLatin1Char('/'))));
                if (!a.isNull())
                    locals.insert(a);
            }
        }
        connAgg.setLocalAddresses(std::move(locals));
    };
    QObject::connect(&ifaceAgg, &InterfaceAggregator::didReset, &app, refreshLocalAddrs);
    QObject::connect(&ifaceAgg, &InterfaceAggregator::rowsChanged, &app,
                     [refreshLocalAddrs](int, int) { refreshLocalAddrs(); });

    // --- ncurses + controller ---
    qiftop::tui::Screen screen;
    screen.init();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&screen] { screen.shutdown(); });

    qiftop::tui::TuiApp tui(&screen, &ifaceAgg, &connAgg, sourceLabel,
                            parser.isSet(themeOpt) ? parser.value(themeOpt) : QString(),
                            parser.isSet(intervalOpt) ? pollMs : 0,
                            parser.isSet(viewOpt) ? parser.value(viewOpt) : QString(),
                            parser.isSet(groupOpt) ? parser.value(groupOpt) : QString());

    // Gate the optional Process/Container columns on the ACTIVE backend's wire
    // tokens — transport-neutral. The in-process Linux conntrack path
    // advertises none (no resolver) → columns stay hidden; in-process BSD
    // attributes → they light up just like the agent.
    const QStringList backendCaps =
        qiftop::backend::mergeCapabilities(netMon.get(), connMon.get());
    tui.setBackendInfo(useAgent, agent.version, backendCaps);

    // The monitors live here, so give the controller a way to push a new poll
    // interval (chosen at runtime in Settings) down to the data source. This
    // also syncs the source to the effective (persisted/CLI) interval now.
    tui.setPollApplier([&netMon, &connMon](int ms) {
        netMon->setPollIntervalMs(ms);
        connMon->setPollIntervalMs(ms);
        netMon->setDesiredIntervalMs(ms);
        connMon->setDesiredIntervalMs(ms);
    });

    // On-demand process details (exe/cmdline/cwd) for the Detail / group-info
    // window: bridge TuiApp → the monitor's GetProcessDetails RPC, and feed
    // async replies back into the controller's cache.
    tui.setProcessDetailsRequester([&connMon](qint32 pid) {
        connMon->requestProcessDetails(pid);
    });
    QObject::connect(connMon.get(), &ConnectionMonitor::processDetailsReady,
                     &tui, &qiftop::tui::TuiApp::onProcessDetails);

    // Input: read raw bytes from stdin ourselves and let Screen decode keys,
    // rather than draining ncurses wgetch() from the notifier. wgetch() under
    // an external event loop is fragile — it returns ERR for both "no input"
    // and EOF (so a closed/redirected stdin spins the level-triggered notifier
    // at 100% CPU), and on FreeBSD ncursesw it fails to consume bytes at all.
    // A direct read() is portable and gives an unambiguous EOF (n == 0): an
    // interactive TUI whose input has closed should exit (top/htop behaviour).
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(
        &stdinNotifier, &QSocketNotifier::activated, &app,
        [&screen, &tui, &stdinNotifier] {
            char buf[256];
            const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                screen.feedInput(buf, static_cast<int>(n));
                int ch;
                while ((ch = screen.pollKey()) != ERR)
                    tui.handleKey(ch);
                return;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                return;            // transient; wait for the next wakeup
            // n == 0 (EOF) or a hard error: stdin is gone — stop the notifier
            // so it can't re-fire on the dead fd, and exit.
            stdinNotifier.setEnabled(false);
            QCoreApplication::quit();
        });

    // --- signal-safe terminal restore + resize ---
    if (::pipe(g_sigPipe) == 0) {
        // Both ends non-blocking: the handler must never block in write(), and
        // the notifier's drain loop relies on read() returning EAGAIN once the
        // pipe is empty — a blocking read end would freeze the event loop
        // inside the callback after the first byte (resize hang + Ctrl-C hang).
        for (int fd : g_sigPipe) {
            int fl = ::fcntl(fd, F_GETFL, 0);
            if (fl != -1)
                ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int fdfl = ::fcntl(fd, F_GETFD, 0);
            if (fdfl != -1)
                ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
        }
        auto *sigNotifier = new QSocketNotifier(g_sigPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(sigNotifier, &QSocketNotifier::activated, &app, [&] {
            unsigned char b;
            while (::read(g_sigPipe[0], &b, 1) > 0) {
                if (b == SIGWINCH) {
                    struct winsize ws{};
                    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col)
                        resizeterm(ws.ws_row, ws.ws_col);
                    tui.requestRedraw();
                } else {
                    screen.shutdown();
                    QCoreApplication::quit();
                }
            }
        });
        std::signal(SIGINT,   signalHandler);
        std::signal(SIGTERM,  signalHandler);
        std::signal(SIGHUP,   signalHandler);
        std::signal(SIGWINCH, signalHandler);
    }

    // Keep the agent warm (its idle manager needs periodic hints). Use the
    // controller's current interval so a runtime change is re-asserted.
    const auto warm = [&] {
        netMon->setDesiredIntervalMs(tui.pollIntervalMs());
        connMon->setDesiredIntervalMs(tui.pollIntervalMs());
    };
    warm();
    auto *heartbeat = new QTimer(&app);
    heartbeat->setInterval(4000);
    QObject::connect(heartbeat, &QTimer::timeout, &app, warm);
    heartbeat->start();

    // Poll interval already applied to the monitors via setPollApplier above.
    netMon->start();
    connMon->start();

    return app.exec();
}
