// qiftop-snapshot-export — one-shot CSV/JSON dump of the current flow table.
//
// Demonstrates the libqiftop export facility (util::exporter + the Exportable
// interface): wire the agent stream through a ConnectionAggregator, wait for a
// couple of polls so rates are meaningful, then serialise the snapshot.
//
//   qiftop-snapshot-export --format csv  > flows.csv
//   qiftop-snapshot-export --format json --output flows.json
//
// Flags:
//   --session        SESSION bus (dev) instead of system
//   --format <fmt>   csv (default) | json
//   --output <path>  write to file (default: stdout)
//   --wait-ms <n>    settle time before snapshot (default 2500)
//
// Numeric columns are emitted as real numbers (not formatted strings) so the
// output is analysable; util::exporter::toCsv also defends against CSV/
// spreadsheet formula injection in attacker-influenced text (hostnames, etc.).

#include <qiftop/aggregate/ConnectionAggregator.h>
#include <qiftop/backend/Connection.h>
#include <qiftop/backend/dbus/DBusConnectionMonitor.h>
#include <qiftop/dbus/Types.h>
#include <qiftop/util/Exportable.h>
#include <qiftop/util/Exporter.h>

#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>

using qiftop::aggregate::ConnectionAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;

namespace {

QString endpoint(const Endpoint &e)
{
    const QString host = e.address.toString();
    return e.isIPv6() ? QStringLiteral("[%1]:%2").arg(host).arg(e.port)
                      : QStringLiteral("%1:%2").arg(host).arg(e.port);
}

// Adapts a captured snapshot of aggregator rows to the Exportable interface
// that util::exporter consumes.
class FlowSnapshot : public Exportable {
public:
    explicit FlowSnapshot(QList<ConnectionAggregator::Row> rows)
        : m_rows(std::move(rows)) {}

    QStringList exportHeaders() const override
    {
        return {QStringLiteral("proto"),  QStringLiteral("local"),
                QStringLiteral("remote"), QStringLiteral("iface"),
                QStringLiteral("rx_rate"), QStringLiteral("tx_rate"),
                QStringLiteral("rx_bytes"), QStringLiteral("tx_bytes"),
                QStringLiteral("comm"),   QStringLiteral("pid"),
                QStringLiteral("uid"),    QStringLiteral("container_runtime"),
                QStringLiteral("container")};
    }

    int exportRowCount() const override { return int(m_rows.size()); }

    QVariantList exportRow(int row) const override
    {
        const ConnectionAggregator::Row &r = m_rows.at(row);
        const Connection &c = r.current;
        const QString container = c.container.valid()
            ? (c.container.name.isEmpty() ? c.container.id : c.container.name)
            : QString();
        return {l4ProtoToString(c.proto), endpoint(c.local), endpoint(c.remote),
                c.iface, r.rxRaw, r.txRaw,
                static_cast<qulonglong>(c.rxBytes), static_cast<qulonglong>(c.txBytes),
                c.process.valid() ? c.process.comm : QString(),
                c.process.valid() ? c.process.pid : 0,
                c.process.valid() ? static_cast<qint64>(c.process.uid) : 0,
                c.container.runtime, container};
    }

private:
    QList<ConnectionAggregator::Row> m_rows;
};

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

    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qiftop::dbus::registerTypes();

    const QStringList args = app.arguments();
    const bool sessionBus  = args.contains(QStringLiteral("--session"));
    const QString format   = argStr(args, QStringLiteral("--format"), QStringLiteral("csv")).toLower();
    const QString output   = argStr(args, QStringLiteral("--output"), QString());
    const int waitMs       = argValue(args, QStringLiteral("--wait-ms"), 2500);

    auto *monitor = new DBusConnectionMonitor(sessionBus, &app);
    auto *agg     = new ConnectionAggregator(&app);
    QObject::connect(monitor, &ConnectionMonitor::connectionsUpdated,
                     agg,     &ConnectionAggregator::updateConnections);
    monitor->setDesiredIntervalMs(1000);
    monitor->start();

    QTimer::singleShot(waitMs, &app, [&] {
        FlowSnapshot snap(agg->rows());
        const QByteArray data = (format == QStringLiteral("json"))
            ? util::exporter::toJson(snap)
            : util::exporter::toCsv(snap);

        int rc = 0;
        if (output.isEmpty()) {
            QTextStream(stdout) << QString::fromUtf8(data);
        } else {
            QString err;
            if (!util::exporter::save(output, data, &err)) {
                QTextStream(stderr) << "qiftop-snapshot-export: " << err << '\n';
                rc = 1;
            } else {
                QTextStream(stderr) << "wrote " << snap.exportRowCount()
                                    << " flows to " << output << '\n';
            }
        }
        app.exit(rc);
    });

    return app.exec();
}
