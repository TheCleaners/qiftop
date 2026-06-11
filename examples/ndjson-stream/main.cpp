// qiftop-ndjson — a ~100-line example consumer of libqiftop that turns the
// qiftop-agent's live DBus stream into NDJSON on stdout. One JSON object
// (a full interface snapshot) per line, so it pipes straight into jq,
// a log shipper, or any line-oriented tool:
//
//   qiftop-ndjson | jq -c '.interfaces[] | select(.rxRate > 1e6)'
//
// Flags:
//   --session   talk to the agent on the SESSION bus (dev) instead of system
//   --once      emit a single snapshot (batch mode) and exit
//
// This is intentionally minimal — it only wires the DBus *interface* monitor
// through an InterfaceAggregator. The same pattern (DBus*Monitor ->
// aggregator -> serialise) extends to ConnectionAggregator for per-flow data,
// and is the seed of the planned Prometheus exporter / alerting daemon: none
// of them need Qt Widgets, only this Core-only library.

#include <qiftop/aggregate/InterfaceAggregator.h>
#include <qiftop/backend/dbus/DBusNetworkMonitor.h>
#include <qiftop/dbus/Types.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using qiftop::aggregate::InterfaceAggregator;
using qiftop::backend::dbus_client::DBusNetworkMonitor;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // QList<InterfaceStats> crosses the monitor's signal/slot edge; register
    // it so queued (cross-thread) delivery works regardless of backend.
    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();

    // Register the DBus wire DTO metatypes with QtDBus, or the agent's
    // StatsChanged reply fails to unmarshal ("type
    // QList<qiftop::dbus::InterfaceStatsDto> is not registered with QtDBus").
    qiftop::dbus::registerTypes();

    const QStringList args = app.arguments();
    const bool sessionBus  = args.contains(QStringLiteral("--session"));
    const bool once        = args.contains(QStringLiteral("--once"));

    auto *monitor = new DBusNetworkMonitor(sessionBus, &app);
    auto *agg     = new InterfaceAggregator(&app);

    QObject::connect(monitor, &NetworkMonitor::statsUpdated,
                     agg,     &InterfaceAggregator::updateStats);

    const auto emitSnapshot = [agg, once, &app] {
        QJsonArray ifaces;
        for (const InterfaceAggregator::Row &r : agg->rows()) {
            ifaces.append(QJsonObject{
                {QStringLiteral("iface"),  r.current.name},
                {QStringLiteral("up"),     r.current.isUp},
                {QStringLiteral("rxRate"), r.rxRate},
                {QStringLiteral("txRate"), r.txRate},
                {QStringLiteral("rxBytes"), static_cast<double>(r.current.rxBytes)},
                {QStringLiteral("txBytes"), static_cast<double>(r.current.txBytes)},
            });
        }
        const QJsonObject line{
            {QStringLiteral("t"),          QDateTime::currentMSecsSinceEpoch()},
            {QStringLiteral("interfaces"), ifaces},
        };
        const QByteArray json = QJsonDocument(line).toJson(QJsonDocument::Compact);
        std::fwrite(json.constData(), 1, static_cast<size_t>(json.size()), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        if (once)
            app.quit();
    };

    QObject::connect(agg, &InterfaceAggregator::didReset, &app, emitSnapshot);
    QObject::connect(agg, &InterfaceAggregator::rowsChanged, &app,
                     [emitSnapshot](int, int) { emitSnapshot(); });

    monitor->setPollIntervalMs(1000);
    monitor->start();

    return app.exec();
}
