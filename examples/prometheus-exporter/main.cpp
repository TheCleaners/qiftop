// qiftop-exporter — a Prometheus / OpenMetrics exporter built on libqiftop.
//
// A ~250-line example consumer that turns the qiftop-agent's live DBus stream
// into a scrapeable /metrics endpoint. It demonstrates that libqiftop is a
// self-contained, Widgets-free data facility: this binary links only
// qiftop::qiftop (+ transitively Qt6::Core/Network/DBus).
//
//   qiftop-exporter --port 9617 &
//   curl -s localhost:9617/metrics | grep qiftop_container
//
// With Prometheus scraping it, "is a container incessantly hogging bandwidth?"
// is a stock alert rule (the `for:` clause IS "incessantly"):
//
//   - alert: ContainerBandwidthHog
//     expr: max_over_time(qiftop_container_tx_rate_bytes{runtime="docker"}[5m]) > 50e6
//     for: 2m
//
// Flags:
//   --session            talk to the agent on the SESSION bus (dev), not system
//   --port <n>           listen port (default 9617)
//   --bind <addr>        bind address (default 0.0.0.0)
//   --interval-ms <n>    desired agent cadence + heartbeat base (default 1000)
//
// Design notes (why these metric types):
//   * Interface byte totals are TRUE monotonic kernel counters -> `_total`
//     counters, safe for PromQL rate().
//   * Per-flow / per-container BYTE totals are NOT exported as counters: flows
//     churn (ephemeral ports) and a sum over currently-open flows is not
//     monotonic (it drops when a flow closes), which would corrupt rate().
//     Instead we export the rates qiftop already computes as GAUGES and let
//     PromQL do avg_over_time()/max_over_time() windows — the correct idiom
//     for an externally-computed rate. This also keeps cardinality bounded to
//     the number of containers/processes, not the number of 5-tuples.

#include <qiftop/aggregate/ConnectionAggregator.h>
#include <qiftop/aggregate/InterfaceAggregator.h>
#include <qiftop/backend/Connection.h>
#include <qiftop/backend/dbus/DBusConnectionMonitor.h>
#include <qiftop/backend/dbus/DBusNetworkMonitor.h>
#include <qiftop/dbus/Types.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QHash>
#include <QHostAddress>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>

using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;
using qiftop::backend::dbus_client::DBusNetworkMonitor;

namespace {

// Escape a Prometheus label value: backslash, double-quote, newline.
QString escapeLabel(const QString &v)
{
    QString out;
    out.reserve(v.size() + 8);
    for (const QChar c : v) {
        if (c == QLatin1Char('\\'))      out += QStringLiteral("\\\\");
        else if (c == QLatin1Char('"'))  out += QStringLiteral("\\\"");
        else if (c == QLatin1Char('\n')) out += QStringLiteral("\\n");
        else                             out += c;
    }
    return out;
}

// A metric family: HELP/TYPE header emitted once, then sample lines.
void emitHeader(QString &out, const char *name, const char *type, const char *help)
{
    out += QStringLiteral("# HELP %1 %2\n").arg(QLatin1String(name), QLatin1String(help));
    out += QStringLiteral("# TYPE %1 %2\n").arg(QLatin1String(name), QLatin1String(type));
}

// One aggregated bucket (per container or per process).
struct Bucket {
    double  rxRate = 0.0;
    double  txRate = 0.0;
    quint64 flows  = 0;
    QString l1;          // primary label value (runtime / comm)
    QString l2;          // secondary label value (container / uid)
};

QString buildMetrics(const InterfaceAggregator *ifAgg, const ConnectionAggregator *connAgg)
{
    QString out;

    // ---- Interfaces: true monotonic counters + rate gauges -----------------
    emitHeader(out, "qiftop_interface_rx_bytes_total", "counter",
               "Total bytes received on the interface (kernel counter).");
    for (const auto &r : ifAgg->rows())
        out += QStringLiteral("qiftop_interface_rx_bytes_total{iface=\"%1\"} %2\n")
                   .arg(escapeLabel(r.current.name))
                   .arg(static_cast<qulonglong>(r.current.rxBytes));
    emitHeader(out, "qiftop_interface_tx_bytes_total", "counter",
               "Total bytes transmitted on the interface (kernel counter).");
    for (const auto &r : ifAgg->rows())
        out += QStringLiteral("qiftop_interface_tx_bytes_total{iface=\"%1\"} %2\n")
                   .arg(escapeLabel(r.current.name))
                   .arg(static_cast<qulonglong>(r.current.txBytes));

    emitHeader(out, "qiftop_interface_rx_rate_bytes", "gauge",
               "Current receive rate on the interface (bytes/sec).");
    for (const auto &r : ifAgg->rows())
        out += QStringLiteral("qiftop_interface_rx_rate_bytes{iface=\"%1\"} %2\n")
                   .arg(escapeLabel(r.current.name)).arg(r.rxRate, 0, 'f', 0);
    emitHeader(out, "qiftop_interface_tx_rate_bytes", "gauge",
               "Current transmit rate on the interface (bytes/sec).");
    for (const auto &r : ifAgg->rows())
        out += QStringLiteral("qiftop_interface_tx_rate_bytes{iface=\"%1\"} %2\n")
                   .arg(escapeLabel(r.current.name)).arg(r.txRate, 0, 'f', 0);

    emitHeader(out, "qiftop_interface_up", "gauge",
               "1 if the interface is administratively up, else 0.");
    for (const auto &r : ifAgg->rows())
        out += QStringLiteral("qiftop_interface_up{iface=\"%1\"} %2\n")
                   .arg(escapeLabel(r.current.name)).arg(r.current.isUp ? 1 : 0);

    // ---- Aggregate connection rates by container and by process ------------
    // Use RAW rates (rxRaw/txRaw), not the smoothed display tween. Bucket key
    // keeps cardinality bounded to #containers / #processes, not #flows.
    QHash<QString, Bucket> byContainer;   // key: runtime/id
    QHash<QString, Bucket> byProcess;     // key: comm/uid
    for (const auto &r : connAgg->rows()) {
        const auto &c = r.current;
        if (c.container.valid()) {
            const QString name = c.container.name.isEmpty() ? c.container.id
                                                            : c.container.name;
            const QString key  = c.container.runtime + QLatin1Char('/') + c.container.id;
            Bucket &b = byContainer[key];
            b.rxRate += r.rxRaw; b.txRate += r.txRaw; ++b.flows;
            b.l1 = c.container.runtime; b.l2 = name;
        }
        if (c.process.valid()) {
            const QString key = c.process.comm + QLatin1Char('/')
                                + QString::number(c.process.uid);
            Bucket &b = byProcess[key];
            b.rxRate += r.rxRaw; b.txRate += r.txRaw; ++b.flows;
            b.l1 = c.process.comm; b.l2 = QString::number(c.process.uid);
        }
    }

    emitHeader(out, "qiftop_container_rx_rate_bytes", "gauge",
               "Aggregated receive rate for the container (bytes/sec).");
    for (const auto &b : byContainer)
        out += QStringLiteral("qiftop_container_rx_rate_bytes{runtime=\"%1\",container=\"%2\"} %3\n")
                   .arg(escapeLabel(b.l1), escapeLabel(b.l2)).arg(b.rxRate, 0, 'f', 0);
    emitHeader(out, "qiftop_container_tx_rate_bytes", "gauge",
               "Aggregated transmit rate for the container (bytes/sec).");
    for (const auto &b : byContainer)
        out += QStringLiteral("qiftop_container_tx_rate_bytes{runtime=\"%1\",container=\"%2\"} %3\n")
                   .arg(escapeLabel(b.l1), escapeLabel(b.l2)).arg(b.txRate, 0, 'f', 0);
    emitHeader(out, "qiftop_container_active_flows", "gauge",
               "Number of active flows attributed to the container.");
    for (const auto &b : byContainer)
        out += QStringLiteral("qiftop_container_active_flows{runtime=\"%1\",container=\"%2\"} %3\n")
                   .arg(escapeLabel(b.l1), escapeLabel(b.l2)).arg(b.flows);

    emitHeader(out, "qiftop_process_rx_rate_bytes", "gauge",
               "Aggregated receive rate for the process (bytes/sec).");
    for (const auto &b : byProcess)
        out += QStringLiteral("qiftop_process_rx_rate_bytes{comm=\"%1\",uid=\"%2\"} %3\n")
                   .arg(escapeLabel(b.l1), escapeLabel(b.l2)).arg(b.rxRate, 0, 'f', 0);
    emitHeader(out, "qiftop_process_tx_rate_bytes", "gauge",
               "Aggregated transmit rate for the process (bytes/sec).");
    for (const auto &b : byProcess)
        out += QStringLiteral("qiftop_process_tx_rate_bytes{comm=\"%1\",uid=\"%2\"} %3\n")
                   .arg(escapeLabel(b.l1), escapeLabel(b.l2)).arg(b.txRate, 0, 'f', 0);

    return out;
}

int argValue(const QStringList &args, const QString &flag, int dflt)
{
    const int i = args.indexOf(flag);
    if (i >= 0 && i + 1 < args.size()) {
        bool ok = false;
        const int v = args.at(i + 1).toInt(&ok);
        if (ok) return v;
    }
    return dflt;
}

QString argStr(const QStringList &args, const QString &flag, const QString &dflt)
{
    const int i = args.indexOf(flag);
    return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : dflt;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();

    // Register the DBus wire DTO metatypes with QtDBus, or the agent's
    // StatsChanged/ConnectionsChanged replies fail to unmarshal
    // ("type QList<qiftop::dbus::...Dto> is not registered with QtDBus").
    qiftop::dbus::registerTypes();

    const QStringList args = app.arguments();
    const bool sessionBus  = args.contains(QStringLiteral("--session"));
    const quint16 port     = static_cast<quint16>(argValue(args, QStringLiteral("--port"), 9617));
    const QString bindStr  = argStr(args, QStringLiteral("--bind"), QStringLiteral("0.0.0.0"));
    const int intervalMs   = argValue(args, QStringLiteral("--interval-ms"), 1000);

    auto *netMon  = new DBusNetworkMonitor(sessionBus, &app);
    auto *connMon = new DBusConnectionMonitor(sessionBus, &app);
    auto *ifAgg   = new InterfaceAggregator(&app);
    auto *connAgg = new ConnectionAggregator(&app);

    QObject::connect(netMon,  &NetworkMonitor::statsUpdated,
                     ifAgg,   &InterfaceAggregator::updateStats);
    QObject::connect(connMon, &ConnectionMonitor::connectionsUpdated,
                     connAgg, &ConnectionAggregator::updateConnections);

    // Ask the agent for our cadence and re-assert on a heartbeat. The agent
    // winds down to idle unless it keeps receiving method calls (hint TTL is
    // ~10s by default); re-assert at ~half-TTL so it never pauses on us.
    netMon->setDesiredIntervalMs(intervalMs);
    connMon->setDesiredIntervalMs(intervalMs);
    netMon->start();
    connMon->start();

    auto *heartbeat = new QTimer(&app);
    heartbeat->setInterval(4000);
    QObject::connect(heartbeat, &QTimer::timeout, &app, [netMon, connMon, intervalMs] {
        netMon->setDesiredIntervalMs(intervalMs);
        connMon->setDesiredIntervalMs(intervalMs);
    });
    heartbeat->start();

    auto *server = new QTcpServer(&app);
    QHostAddress bindAddr;
    if (!bindAddr.setAddress(bindStr)) bindAddr = QHostAddress::Any;
    if (!server->listen(bindAddr, port)) {
        QTextStream(stderr) << "qiftop-exporter: failed to listen on "
                            << bindStr << ':' << port << " — "
                            << server->errorString() << '\n';
        return 1;
    }
    QTextStream(stderr) << "qiftop-exporter: serving /metrics on "
                        << bindStr << ':' << port << '\n';

    QObject::connect(server, &QTcpServer::newConnection, &app, [server, ifAgg, connAgg] {
        while (QTcpSocket *sock = server->nextPendingConnection()) {
            QObject::connect(sock, &QTcpSocket::readyRead, sock, [sock, ifAgg, connAgg] {
                const QByteArray req = sock->readAll();
                const bool isMetrics = req.startsWith("GET /metrics");
                const bool isHealth  = req.startsWith("GET /healthz")
                                       || req.startsWith("GET / ");
                QByteArray body;
                QByteArray status = "200 OK";
                QByteArray ctype  = "text/plain; version=0.0.4; charset=utf-8";
                if (isMetrics) {
                    body = buildMetrics(ifAgg, connAgg).toUtf8();
                } else if (isHealth) {
                    body = "ok\n";
                } else {
                    status = "404 Not Found";
                    body   = "not found\n";
                }
                QByteArray resp = "HTTP/1.1 " + status + "\r\n"
                                  "Content-Type: " + ctype + "\r\n"
                                  "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                                  "Connection: close\r\n\r\n" + body;
                sock->write(resp);
                sock->flush();
                sock->waitForBytesWritten(2000);
                sock->disconnectFromHost();
            });
            QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    });

    return app.exec();
}
