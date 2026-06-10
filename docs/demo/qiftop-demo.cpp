// qiftop-demo — a privacy-safe, fully synthetic driver for the qiftop GUI.
//
// Builds the REAL MainWindow (same models, delegates, gauges, colours as the
// shipping app) but feeds it from the in-memory test fakes instead of a live
// kernel / conntrack / DBus agent. Every interface, address, hostname, pid and
// container shown here is fabricated:
//   * addresses use the reserved documentation ranges (RFC 5737 TEST-NET-1/2/3
//     192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24, and RFC 3849 2001:db8::/32)
//   * hostnames use the reserved example.* domains (RFC 2606)
// so a screen capture of this binary leaks NOTHING about the host it runs on.
//
// Built only when -DQIFTOP_BUILD_DEMO=ON. Intended for generating the README
// screenshot / animated capture under a headless X server (Xvfb), e.g.:
//   Xvfb :99 -screen 0 1280x760x24 &
//   DISPLAY=:99 QT_QPA_PLATFORM=xcb QT_STYLE_OVERRIDE=Adwaita-Dark \
//       ./qiftop-demo --script
//
// Flags:
//   --width N --height N   window size (default 1280x720)
//   --script               run the scripted view-mode tour (Flat → ByContainer
//                          → ByProcess) on a loop, for an unattended capture
//   --dark                 apply a built-in Fusion dark palette (fallback for
//                          when no QT_STYLE_OVERRIDE dark theme is installed)

#include <QApplication>
#include <QHostAddress>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <vector>

#include "config/Settings.h"
#include "ui/MainWindow.h"

#include "backend/Connection.h"
#include "backend/NetworkMonitor.h"
#include "backend/ConnectionMonitor.h"

// The reusable in-memory fakes that the widget tests use (on the include path
// via target_include_directories(${CMAKE_SOURCE_DIR}/tests)).
#include "fakes/FakeMonitors.h"

using namespace qiftop;
using namespace qiftop::tests;
using qiftop::backend::ContainerInfo;
using qiftop::backend::ProcessInfo;

namespace {

// A synthetic interface whose cumulative counters advance every tick at a
// target rate (so the model derives a realistic, animated throughput).
struct DemoIface {
    InterfaceStats base;
    double rxRate;   // bytes/sec target
    double txRate;
    double phase;    // for gentle oscillation
    quint64 rx = 0;
    quint64 tx = 0;
};

// A synthetic flow with attribution + an advancing byte counter.
struct DemoFlow {
    Connection c;
    double rxRate;
    double txRate;
    double phase;
    quint64 rx = 0;
    quint64 tx = 0;
};

Connection mkAttributedFlow(const char *iface, quint32 ifIndex,
                            const char *local, quint16 lport,
                            const char *remote, quint16 rport,
                            L4Proto proto, Direction dir,
                            const ProcessInfo &process,
                            const ContainerInfo &container = {},
                            QList<ContainerInfo> chain = {})
{
    Connection c;
    c.proto          = proto;
    c.iface          = QString::fromLatin1(iface);
    c.ifIndex        = ifIndex;
    c.local.address  = QHostAddress(QString::fromLatin1(local));
    c.local.port     = lport;
    c.remote.address = QHostAddress(QString::fromLatin1(remote));
    c.remote.port    = rport;
    c.direction      = dir;
    c.tcpState       = (proto == L4Proto::Tcp) ? TcpState::Established
                                               : TcpState::None;
    c.process        = process;
    c.container      = container;
    c.containerChain = std::move(chain);
    return c;
}

ProcessInfo mkProc(qint32 pid, const char *comm, quint32 uid,
                   const char *exe = nullptr)
{
    ProcessInfo p;
    p.pid  = pid;
    p.comm = QString::fromLatin1(comm);
    p.uid  = uid;
    if (exe) p.exe = QString::fromLatin1(exe);
    return p;
}

ContainerInfo mkCont(const char *runtime, const char *id, const char *name)
{
    ContainerInfo ci;
    ci.runtime = QString::fromLatin1(runtime);
    ci.id      = QString::fromLatin1(id);
    ci.name    = QString::fromLatin1(name);
    return ci;
}

// A self-contained dark palette so the capture is reliably dark even when no
// Adwaita-Dark / gnome platform theme is installed (CI, minimal containers).
void applyDarkPalette(QApplication &app)
{
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion")))
        app.setStyle(QStringLiteral("Fusion"));
    QPalette p;
    const QColor base(30, 31, 34), alt(36, 37, 41), text(220, 221, 224),
                 window(24, 25, 28), btn(45, 46, 51), hl(45, 125, 210);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, btn);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, hl);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 121, 125));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 121, 125));
    app.setPalette(p);
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("qiftop-demo"));

    int width = 1280, height = 720;
    bool script = false, dark = false;
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--width") && i + 1 < args.size())
            width = args[++i].toInt();
        else if (args[i] == QLatin1String("--height") && i + 1 < args.size())
            height = args[++i].toInt();
        else if (args[i] == QLatin1String("--script"))
            script = true;
        else if (args[i] == QLatin1String("--dark"))
            dark = true;
    }
    if (dark)
        applyDarkPalette(app);

    // Ensure themed toolbar/menu icons resolve even with no platform theme
    // (headless capture sets no icon theme, so QIcon::fromTheme would return
    // null and the toolbar would fall back to text). Only set when nothing
    // else has, so a real desktop's theme still wins.
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("Adwaita"));
    if (QIcon::fallbackThemeName().isEmpty())
        QIcon::setFallbackThemeName(QStringLiteral("Adwaita"));

    // Metatypes the models pass through QVariant / cross-thread queues.
    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qRegisterMetaType<qiftop::backend::ProcessDetails>();

    // Isolate settings in a throwaway dir so we neither read nor write the
    // real user's qiftop config (which could carry host-specific state).
    QTemporaryDir cfgDir;
    qputenv("XDG_CONFIG_HOME", cfgDir.path().toUtf8());

    Settings settings;
    // Show off the v0.2 attribution columns (capability-gated below) and the
    // signature per-row throughput gauge + Max columns.
    settings.setShowProcessColumn(true);
    settings.setShowContainerColumn(true);
    settings.setThroughputGaugeEnabled(true);
    // Direction-tinted gauges (green inbound / amber outbound) — the signature
    // colour coding; without this the gauge fill is a neutral gray.
    settings.setTintRowByDirection(true);
    // Resolve hostnames where we have a (synthetic) DNS answer; the rest stay
    // as raw IPs — a realistic mix.
    settings.setResolveHostnames(true);
    settings.setConnectionViewMode(Settings::ConnectionViewMode::Flat);

    FakeNetworkMonitor    netMon;
    FakeConnectionMonitor connMon;
    FakeDnsResolver       dns;

    MainWindow win(&settings, &netMon, &connMon, &dns);
    // Pretend we're talking to a full-featured agent so the Process /
    // Container columns un-gate (applySettingsToUi AND-gates pref ∧ wire cap).
    win.setBackendInfo(true, QStringLiteral("0.5"), QStringList{
        QStringLiteral("iana-proto"), QStringLiteral("direction-on-wire"),
        QStringLiteral("tcp-state"), QStringLiteral("ifindex"),
        QStringLiteral("process-attribution-wire"),
        QStringLiteral("container-attribution-wire"),
        QStringLiteral("container-chain-wire"),
        QStringLiteral("on-demand-process-details"),
    });
    win.resize(width, height);
    win.show();
    // Open on the Connections view — that's where the flows, attribution
    // columns, throughput gauges and colour coding live.
    win.selectConnectionsTab();

    // --- Synthetic dataset (all fabricated; doc-reserved addresses) ---------
    std::vector<DemoIface> ifaces;
    ifaces.push_back({mkIface("enp3s0", 2), 9.0e5, 1.4e5, 0.0, 0, 0});
    ifaces.push_back({mkIface("wlan0",  3), 3.1e5, 5.2e4, 1.1, 0, 0});
    ifaces.push_back({mkIface("docker0",4), 2.0e5, 1.9e5, 2.3, 0, 0});
    // Give the host its own (synthetic) addresses so the model knows which
    // endpoint is local — otherwise every flow looks "forwarded" and the
    // direction-tinted gauge collapses to a single neutral amber. CIDR
    // strings, exactly as the real NetworkMonitor emits them.
    ifaces[0].base.addresses << QStringLiteral("192.0.2.50/24");
    ifaces[1].base.addresses << QStringLiteral("2001:db8:1::50/64");

    // Stage friendly hostnames for the synthetic peers (example.* per RFC 2606).
    auto stageDns = [&](const char *ip, const char *name) {
        dns.setCached(QHostAddress(QString::fromLatin1(ip)),
                      QString::fromLatin1(name));
    };
    stageDns("203.0.113.20", "cdn.example.com");
    stageDns("203.0.113.45", "api.example.net");
    stageDns("198.51.100.30", "registry.example.io");
    stageDns("192.0.2.200",  "db.internal.example");
    stageDns("2001:db8:2::1", "ipv6.example.com");
    // Deliberately leave 198.51.100.77 and 203.0.113.99 unresolved so they
    // render as raw IPs — not every peer has a PTR record in real life.

    std::vector<DemoFlow> flows;
    // Host processes (no container).
    flows.push_back({mkAttributedFlow("enp3s0", 2, "192.0.2.50", 51344,
                        "203.0.113.20", 443, L4Proto::Tcp, Direction::Inbound,
                        mkProc(2451, "firefox", 1000, "/usr/lib/firefox/firefox")),
                     8.6e5, 4.2e4, 0.0, 0, 0});
    flows.push_back({mkAttributedFlow("wlan0", 3, "192.0.2.50", 49920,
                        "203.0.113.99", 443, L4Proto::Tcp, Direction::Inbound,
                        mkProc(2451, "firefox", 1000, "/usr/lib/firefox/firefox")),
                     2.7e5, 2.6e4, 0.7, 0, 0});
    flows.push_back({mkAttributedFlow("enp3s0", 2, "192.0.2.50", 50112,
                        "198.51.100.77", 443, L4Proto::Tcp, Direction::Outbound,
                        mkProc(3310, "curl", 1000, "/usr/bin/curl")),
                     3.0e4, 1.9e5, 1.9, 0, 0});
    // Containerised workloads.
    flows.push_back({mkAttributedFlow("docker0", 4, "192.0.2.50", 33456,
                        "198.51.100.30", 443, L4Proto::Tcp, Direction::Outbound,
                        mkProc(8841, "node", 1000, "/usr/local/bin/node"),
                        mkCont("docker", "a3f9c1d2e4b6", "web-frontend")),
                     4.4e4, 1.7e5, 2.5, 0, 0});
    flows.push_back({mkAttributedFlow("docker0", 4, "192.0.2.50", 33890,
                        "192.0.2.200", 5432, L4Proto::Tcp, Direction::Outbound,
                        mkProc(8902, "postgres", 999, "/usr/lib/postgresql/postgres"),
                        mkCont("containerd", "7d52b8a0f3c1", "orders-db"),
                        QList<ContainerInfo>{
                            mkCont("kubernetes", "pod-7f9c", "orders"),
                            mkCont("containerd", "7d52b8a0f3c1", "orders-db")}),
                     1.9e5, 3.3e4, 0.4, 0, 0});
    flows.push_back({mkAttributedFlow("docker0", 4, "192.0.2.50", 34002,
                        "203.0.113.45", 443, L4Proto::Tcp, Direction::Inbound,
                        mkProc(9120, "python3", 1000, "/usr/bin/python3"),
                        mkCont("podman", "c4e1a9b7d8f2", "scraper")),
                     2.3e5, 1.8e4, 1.3, 0, 0});
    // A couple of UDP/DNS + IPv6 flows for variety.
    flows.push_back({mkAttributedFlow("enp3s0", 2, "192.0.2.50", 41020,
                        "198.51.100.30", 53, L4Proto::Udp, Direction::Outbound,
                        mkProc(1180, "systemd-resolve", 101)),
                     1.2e3, 9.0e2, 3.1, 0, 0});
    flows.push_back({mkAttributedFlow("wlan0", 3, "2001:db8:1::50", 52771,
                        "2001:db8:2::1", 443, L4Proto::Tcp, Direction::Inbound,
                        mkProc(2451, "firefox", 1000, "/usr/lib/firefox/firefox")),
                     1.5e5, 1.7e4, 2.0, 0, 0});

    // --- Animate: advance counters at each flow's target rate (+ gentle
    //     oscillation) so the gauges and rate columns move like the real app.
    constexpr int kTickMs = 500;
    constexpr double kDt = kTickMs / 1000.0;
    auto *timer = new QTimer(&win);
    auto t = std::make_shared<double>(0.0);
    QObject::connect(timer, &QTimer::timeout, &win, [&netMon, &connMon,
                                                     &ifaces, &flows, t] {
        *t += kDt;
        QList<InterfaceStats> istats;
        for (auto &di : ifaces) {
            const double osc = 0.75 + 0.25 * std::sin(*t * 0.6 + di.phase);
            di.rx += static_cast<quint64>(di.rxRate * osc * kDt);
            di.tx += static_cast<quint64>(di.txRate * osc * kDt);
            InterfaceStats s = di.base;
            s.rxBytes   = di.rx;
            s.txBytes   = di.tx;
            s.rxPackets = di.rx / 1400;
            s.txPackets = di.tx / 1400;
            istats << s;
        }
        netMon.emitSnapshot(istats);

        QList<Connection> conns;
        for (auto &df : flows) {
            const double osc = 0.7 + 0.3 * std::sin(*t * 0.9 + df.phase);
            df.rx += static_cast<quint64>(df.rxRate * osc * kDt);
            df.tx += static_cast<quint64>(df.txRate * osc * kDt);
            Connection c = df.c;
            c.rxBytes   = df.rx;
            c.txBytes   = df.tx;
            c.rxPackets = df.rx / 1400;
            c.txPackets = df.tx / 1400;
            conns << c;
        }
        connMon.emitSnapshot(conns);
    });
    timer->start(kTickMs);

    // Optional scripted tour: Flat → By Container → By Process, looping, so an
    // unattended capture shows the grouping modes too.
    if (script) {
        auto *tour = new QTimer(&win);
        auto step = std::make_shared<int>(0);
        QObject::connect(tour, &QTimer::timeout, &win, [&settings, step] {
            switch (*step % 3) {
            case 0: settings.setConnectionViewMode(
                        Settings::ConnectionViewMode::Flat); break;
            case 1: settings.setConnectionViewMode(
                        Settings::ConnectionViewMode::ByContainer); break;
            case 2: settings.setConnectionViewMode(
                        Settings::ConnectionViewMode::ByProcess); break;
            }
            ++*step;
        });
        tour->start(4000);
    }

    return app.exec();
}
