// qiftop-top — a headless "iftop -t"-style top-talkers printer.
//
// Every interval, prints the top-N flows by current total rate, sorted, using
// libqiftop's aggregator + IEC rate formatter. A tiny demonstration that the
// library is enough to build a text UI with no Widgets and no kernel access.
//
//   qiftop-top --top 15 --interval-ms 2000
//
// Flags:
//   --session       SESSION bus (dev) instead of system
//   --top <n>       rows to show (default 10)
//   --interval-ms   refresh cadence (default 2000)

#include <qiftop/aggregate/ConnectionAggregator.h>
#include <qiftop/backend/Connection.h>
#include <qiftop/backend/dbus/DBusConnectionMonitor.h>
#include <qiftop/dbus/Types.h>
#include <qiftop/util/Units.h>

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

#include <algorithm>

using qiftop::aggregate::ConnectionAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;

namespace {

QString endpoint(const Endpoint &e)
{
    const QString host = e.address.toString();
    return e.isIPv6() ? QStringLiteral("[%1]:%2").arg(host).arg(e.port)
                      : QStringLiteral("%1:%2").arg(host).arg(e.port);
}

QString ownerLabel(const Connection &c)
{
    if (c.container.valid())
        return (c.container.name.isEmpty() ? c.container.id : c.container.name)
               + QStringLiteral(" (") + c.container.runtime + QLatin1Char(')');
    if (c.process.valid())
        return c.process.comm;
    return QStringLiteral("-");
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
    const int topN         = std::max(1, argValue(args, QStringLiteral("--top"), 10));
    const int intervalMs   = argValue(args, QStringLiteral("--interval-ms"), 2000);

    auto *monitor = new DBusConnectionMonitor(sessionBus, &app);
    auto *agg     = new ConnectionAggregator(&app);
    QObject::connect(monitor, &ConnectionMonitor::connectionsUpdated,
                     agg,     &ConnectionAggregator::updateConnections);
    monitor->setDesiredIntervalMs(intervalMs);
    monitor->start();

    auto *timer = new QTimer(&app);
    timer->setInterval(intervalMs);
    QObject::connect(timer, &QTimer::timeout, &app, [agg, topN] {
        QList<ConnectionAggregator::Row> rows = agg->rows();
        std::sort(rows.begin(), rows.end(),
                  [](const ConnectionAggregator::Row &a, const ConnectionAggregator::Row &b) {
                      return (a.rxRaw + a.txRaw) > (b.rxRaw + b.txRaw);
                  });

        QTextStream out(stdout);
        out << "\x1b[2J\x1b[H";   // clear + home
        out << QStringLiteral("%1  %2  %3  %4  %5\n")
                   .arg(QStringLiteral("PROTO"), -5)
                   .arg(QStringLiteral("LOCAL → REMOTE"), -46)
                   .arg(QStringLiteral("OWNER"), -22)
                   .arg(QStringLiteral("RX"), 12)
                   .arg(QStringLiteral("TX"), 12);
        const int n = std::min(topN, int(rows.size()));
        for (int i = 0; i < n; ++i) {
            const Connection &c = rows.at(i).current;
            const QString flow = endpoint(c.local) + QStringLiteral(" → ") + endpoint(c.remote);
            out << QStringLiteral("%1  %2  %3  %4  %5\n")
                       .arg(l4ProtoToString(c.proto), -5)
                       .arg(flow, -46)
                       .arg(ownerLabel(c).left(22), -22)
                       .arg(util::formatByteRate(rows.at(i).rxRaw), 12)
                       .arg(util::formatByteRate(rows.at(i).txRaw), 12);
        }
        out.flush();
    });
    timer->start();

    return app.exec();
}
