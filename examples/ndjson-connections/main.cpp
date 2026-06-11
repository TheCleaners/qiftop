// qiftop-ndjson-connections — per-flow NDJSON from the qiftop-agent stream.
//
// Sibling of examples/ndjson-stream (which does interfaces): this one emits
// ONE JSON object PER FLOW PER LINE, including the v0.2 process/container
// attribution, so it pipes straight into jq or a log shipper:
//
//   qiftop-ndjson-connections | jq -c 'select(.tx_rate > 1e6)'
//   qiftop-ndjson-connections | jq -c 'select(.container=="db")'
//
// Flags:
//   --session       talk to the agent on the SESSION bus (dev), not system
//   --interval-ms   snapshot cadence (default 1000)
//   --once          emit one snapshot and exit
//
// Links only qiftop::qiftop — no Qt Widgets, no kernel access.

#include <qiftop/aggregate/ConnectionAggregator.h>
#include <qiftop/backend/Connection.h>
#include <qiftop/backend/dbus/DBusConnectionMonitor.h>
#include <qiftop/dbus/Types.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>

using qiftop::aggregate::ConnectionAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;

namespace {

QString endpoint(const Endpoint &e)
{
    const QString host = e.address.toString();
    return e.isIPv6() ? QStringLiteral("[%1]:%2").arg(host).arg(e.port)
                      : QStringLiteral("%1:%2").arg(host).arg(e.port);
}

const char *directionStr(Direction d)
{
    switch (d) {
    case Direction::Outbound: return "out";
    case Direction::Inbound:  return "in";
    default:                  return "unknown";
    }
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qiftop::dbus::registerTypes();

    const QStringList args = app.arguments();
    const bool sessionBus  = args.contains(QStringLiteral("--session"));
    const bool once        = args.contains(QStringLiteral("--once"));
    const int intervalMs   = argValue(args, QStringLiteral("--interval-ms"), 1000);

    auto *monitor = new DBusConnectionMonitor(sessionBus, &app);
    auto *agg     = new ConnectionAggregator(&app);

    QObject::connect(monitor, &ConnectionMonitor::connectionsUpdated,
                     agg,     &ConnectionAggregator::updateConnections);

    const auto emitSnapshot = [agg, once, &app] {
        const qint64 t = QDateTime::currentMSecsSinceEpoch();
        for (const ConnectionAggregator::Row &r : agg->rows()) {
            const Connection &c = r.current;
            QJsonObject o{
                {QStringLiteral("t"),        t},
                {QStringLiteral("proto"),    l4ProtoToString(c.proto)},
                {QStringLiteral("local"),    endpoint(c.local)},
                {QStringLiteral("remote"),   endpoint(c.remote)},
                {QStringLiteral("iface"),    c.iface},
                {QStringLiteral("direction"), QLatin1String(directionStr(c.direction))},
                {QStringLiteral("rx_rate"),  r.rxRaw},
                {QStringLiteral("tx_rate"),  r.txRaw},
                {QStringLiteral("rx_bytes"), static_cast<double>(c.rxBytes)},
                {QStringLiteral("tx_bytes"), static_cast<double>(c.txBytes)},
            };
            if (c.process.valid()) {
                o.insert(QStringLiteral("comm"), c.process.comm);
                o.insert(QStringLiteral("pid"),  c.process.pid);
                o.insert(QStringLiteral("uid"),  static_cast<qint64>(c.process.uid));
            }
            if (c.container.valid()) {
                o.insert(QStringLiteral("container_runtime"), c.container.runtime);
                o.insert(QStringLiteral("container"),
                         c.container.name.isEmpty() ? c.container.id : c.container.name);
            }
            const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact);
            std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
            std::fputc('\n', stdout);
        }
        std::fflush(stdout);
        if (once)
            app.quit();
    };

    auto *timer = new QTimer(&app);
    timer->setInterval(intervalMs);
    QObject::connect(timer, &QTimer::timeout, &app, emitSnapshot);

    monitor->setDesiredIntervalMs(intervalMs);
    monitor->start();

    if (once) {
        // Need two polls before rates are meaningful; emit after a short grace.
        QTimer::singleShot(intervalMs * 2 + 500, &app, emitSnapshot);
    } else {
        timer->start();
    }

    return app.exec();
}
