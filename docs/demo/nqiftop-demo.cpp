// nqiftop-demo — synthetic, privacy-safe driver for the ncurses front-end,
// used to record the README terminal GIF. Gated behind -DQIFTOP_BUILD_DEMO=ON;
// never built by default, never installed.
//
// Like docs/demo/qiftop-demo.cpp (the GUI variant) nothing here touches a live
// kernel, conntrack table, or DBus agent. It wires the real Screen + TuiApp to
// the in-memory FakeMonitors and feeds a curated, fabricated dataset:
//   * addresses: RFC 5737 TEST-NET (192.0.2/24, 198.51.100/24, 203.0.113/24)
//     and RFC 3849 (2001:db8::/32),
//   * process / container attribution: invented comms, pids and container ids.
// A scripted key tour (driven internally via TuiApp::handleKey, so capture is
// deterministic and needs no xdotool) shows the headline features: the
// row-spanning gauge, grouping by process / container, the modal aptitude-style
// detail inspector (Enter opens, j/k browse adjacent rows, any key closes),
// the settings panel and live theme cycling.

#include <QCoreApplication>
#include <QList>
#include <QRandomGenerator>
#include <QTimer>

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "tui/Screen.h"
#include "tui/TuiApp.h"

#include "fakes/FakeMonitors.h"

using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;

namespace {

// One fabricated flow plus its target bandwidth (bytes/s) so the feed can
// animate growing byte counters → non-zero, varying rates.
struct FlowSpec {
    const char *iface;
    const char *localAddr;  quint16 localPort;
    const char *remoteAddr; quint16 remotePort;
    L4Proto     proto;
    const char *comm;       qint32  pid;   quint32 uid;
    const char *runtime;    const char *cid; const char *cname; // "" = none
    double      rxBps, txBps;
    quint64     rx = 0, tx = 0;   // running counters (mutated each tick)
};

// Curated synthetic dataset. Mix of v4/v6, TCP/UDP, attributed to a few
// fake processes and containers so the grouping views are interesting.
QList<FlowSpec> makeFlows()
{
    return {
        {"eth0", "192.0.2.10", 44102, "203.0.113.20", 443, L4Proto::Tcp,
         "firefox", 2207, 1000, "", "", "", 920000, 64000},
        {"eth0", "192.0.2.10", 51020, "198.51.100.7", 443, L4Proto::Tcp,
         "firefox", 2207, 1000, "", "", "", 410000, 38000},
        {"eth0", "192.0.2.10", 38844, "203.0.113.51", 51820, L4Proto::Udp,
         "wireguard", 311, 0, "", "", "", 180000, 220000},
        {"docker0", "192.0.2.10", 33112, "198.51.100.40", 5432, L4Proto::Tcp,
         "postgres", 4801, 999, "docker", "9f3c1a2b7d4e", "db", 64000, 240000},
        {"docker0", "192.0.2.10", 33950, "203.0.113.88", 6379, L4Proto::Tcp,
         "redis-server", 4990, 999, "docker", "1b77e0c9a233", "cache", 30000, 41000},
        {"eth0", "2001:db8::10", 49210, "2001:db8:1f::25", 443, L4Proto::Tcp,
         "curl", 6120, 1000, "podman", "c0ffee123456", "build", 1450000, 22000},
        {"eth0", "192.0.2.10", 5353, "198.51.100.1", 53, L4Proto::Udp,
         "systemd-resolve", 740, 101, "", "", "", 2200, 3100},
        {"wg0", "192.0.2.77", 41666, "203.0.113.200", 443, L4Proto::Tcp,
         "ssh", 8233, 1000, "", "", "", 14000, 9000},
    };
}

Connection toConnection(const FlowSpec &f)
{
    Connection c = qiftop::tests::mkFlow(f.iface, f.localAddr, f.localPort,
                                         f.remoteAddr, f.remotePort, f.proto,
                                         f.rx, f.tx, Direction::Outbound);
    c.tcpState = (f.proto == L4Proto::Tcp) ? TcpState::Established : TcpState::None;
    c.process.pid = f.pid;
    c.process.uid = f.uid;
    c.process.comm = QString::fromLatin1(f.comm);
    if (f.runtime[0]) {
        c.container.runtime = QString::fromLatin1(f.runtime);
        c.container.id      = QString::fromLatin1(f.cid);
        c.container.name    = QString::fromLatin1(f.cname);
        c.containerChain << c.container;
    }
    return c;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("nqiftop-demo"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.1"));

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();

    InterfaceAggregator  ifaceAgg;
    ConnectionAggregator connAgg;
    connAgg.setRateSmoothingMs(300);
    connAgg.setUdpAggregateByPeer(false);

    qiftop::tests::FakeNetworkMonitor    netMon;
    qiftop::tests::FakeConnectionMonitor connMon;
    QObject::connect(&netMon, &NetworkMonitor::statsUpdated,
                     &ifaceAgg, &InterfaceAggregator::updateStats);
    QObject::connect(&connMon, &ConnectionMonitor::connectionsUpdated,
                     &connAgg, &ConnectionAggregator::updateConnections);

    qiftop::tui::Screen screen;
    screen.init();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&screen] { screen.shutdown(); });

    // Start on Connections / ungrouped so the tour is deterministic regardless
    // of any persisted state (XDG_CONFIG_HOME is sandboxed by the capture script).
    qiftop::tui::TuiApp tui(&screen, &ifaceAgg, &connAgg,
                            QStringLiteral("demo (synthetic)"),
                            QStringLiteral("dark"), 1000,
                            QStringLiteral("connections"), QStringLiteral("off"));

    // Per-interface synthetic counters, grown from the flows that use them.
    struct IfaceCtr { quint64 rx = 0, tx = 0; };
    auto flows = makeFlows();

    const auto emitSnapshot = [&] {
        QHash<QString, IfaceCtr> ifc;
        QList<Connection> conns;
        for (auto &f : flows) {
            // ~1s of bandwidth + light jitter so rates wobble like real traffic.
            const double j = 0.75 + (QRandomGenerator::global()->bounded(50) / 100.0);
            f.rx += static_cast<quint64>(f.rxBps * j);
            f.tx += static_cast<quint64>(f.txBps * j);
            ifc[QString::fromLatin1(f.iface)].rx += f.rx;
            ifc[QString::fromLatin1(f.iface)].tx += f.tx;
            conns << toConnection(f);
        }
        QList<InterfaceStats> ifaces;
        quint32 idx = 1;
        for (auto it = ifc.constBegin(); it != ifc.constEnd(); ++it) {
            InterfaceStats s = qiftop::tests::mkIface(it.key().toLatin1().constData(),
                                                      idx++, it.value().rx, it.value().tx);
            if (it.key() == QLatin1String("docker0"))
                s.type = QStringLiteral("bridge");
            else if (it.key() == QLatin1String("wg0"))
                s.type = QStringLiteral("tun");
            s.mtu = (it.key() == QLatin1String("wg0")) ? 1420 : 1500;
            s.addresses << QStringLiteral("192.0.2.10/24");
            ifaces << s;
        }
        netMon.emitSnapshot(ifaces);
        connMon.emitSnapshot(conns);
    };

    auto *feed = new QTimer(&app);
    feed->setInterval(1000);
    QObject::connect(feed, &QTimer::timeout, &app, emitSnapshot);
    emitSnapshot();           // seed
    QTimer::singleShot(900, &app, emitSnapshot); // second sample → first rates
    feed->start();

    // Scripted key tour. Each entry is a curses key fed to TuiApp::handleKey
    // on a fixed cadence so the recording is reproducible.
    struct Step { int afterMs; int key; };
    const QList<Step> tour = {
        {2600, 'g'},                 // group by interface
        {1500, 'g'},                 // group by process
        {1800, 'j'}, {500, 'j'},     // move cursor onto a flow
        {1200, '\n'},                // open the modal detail inspector
        {1600, 'j'}, {1400, 'j'},    // browse next rows' details (modal re-targets)
        {1500, 'k'},                 // browse back up
        {1600, '\n'},                // close the detail overlay
        {1200, 'g'},                 // group by container
        {2400, 'S'},                 // open Settings
        {1200, 'j'}, {500, 'j'},     // move to "Bandwidth gauge"
        {1200, 'S'},                 // close Settings
        {1200, 'z'},                 // cycle theme (light)
        {1600, 'z'},                 // cycle theme (colorblind)
        {1600, '1'},                 // Interfaces tab
        {1500, '\n'},                // inspect an interface (detail overlay)
        {2200, '\n'},                // close
        {1200, '2'},                 // back to Connections
        {1200, 'g'},                 // ungroup (back to flat)
        {2200, 'z'},                 // back toward dark
        {1600, 'q'},                 // quit
    };
    int t = 0;
    for (const Step &s : tour) {
        t += s.afterMs;
        const int key = s.key;
        QTimer::singleShot(t, &app, [&tui, key] { tui.handleKey(key); });
    }

    return app.exec();
}
