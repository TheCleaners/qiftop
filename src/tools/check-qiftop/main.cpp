#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"
#include "backend/dbus/DBusConnectionMonitor.h"
#include "backend/dbus/DBusNetworkMonitor.h"
#include "dbus/Types.h"
#include "util/ConnectionFilter.h"
#include "util/Units.h"

#include <QCoreApplication>
#include <QList>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <optional>

using qiftop::aggregate::ConnectionAggregator;
using qiftop::aggregate::InterfaceAggregator;
using qiftop::backend::dbus_client::DBusConnectionMonitor;
using qiftop::backend::dbus_client::DBusNetworkMonitor;

namespace {

enum ExitCode {
    Ok       = 0,
    Warning  = 1,
    Critical = 2,
    Unknown  = 3,
};

enum class MetricPart { Rx, Tx, Total };

struct Options {
    MetricPart part = MetricPart::Total;
    QString metricArg = QStringLiteral("rate_total");
    QString iface;
    QString filter;
    double warning = 0.0;
    double critical = 0.0;
    bool haveWarning = false;
    bool haveCritical = false;
    bool sessionBus = false;
    int timeoutSec = 5;
    int intervalMs = 1000;
};

QString compacted(QString s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : std::as_const(s)) {
        if (!c.isSpace())
            out += c;
    }
    return out;
}

std::optional<double> parseByteRate(const QString &input)
{
    QString s = compacted(input.trimmed());
    if (s.isEmpty())
        return std::nullopt;
    if (s.endsWith(QStringLiteral("/s"), Qt::CaseInsensitive))
        s.chop(2);

    struct Suffix {
        const char *text;
        double multiplier;
    };
    static constexpr Suffix suffixes[] = {
        {"tib", 1099511627776.0}, {"tb", 1000000000000.0}, {"ti", 1099511627776.0}, {"t", 1000000000000.0},
        {"gib", 1073741824.0},    {"gb", 1000000000.0},    {"gi", 1073741824.0},    {"g", 1000000000.0},
        {"mib", 1048576.0},       {"mb", 1000000.0},       {"mi", 1048576.0},       {"m", 1000000.0},
        {"kib", 1024.0},          {"kb", 1000.0},          {"ki", 1024.0},          {"k", 1000.0},
        {"bytes", 1.0},           {"byte", 1.0},           {"b", 1.0},
    };

    double multiplier = 1.0;
    const QString lower = s.toLower();
    for (const auto &suffix : suffixes) {
        const QString suf = QString::fromLatin1(suffix.text);
        if (lower.endsWith(suf)) {
            s.chop(suf.size());
            multiplier = suffix.multiplier;
            break;
        }
    }

    bool ok = false;
    const double n = s.toDouble(&ok);
    if (!ok || n < 0.0)
        return std::nullopt;
    return n * multiplier;
}

std::optional<MetricPart> parseMetric(const QString &metric)
{
    const QString m = metric.trimmed().toLower();
    if (m == QStringLiteral("rate_in") || m == QStringLiteral("rx_rate")
        || m == QStringLiteral("rate_rx") || m == QStringLiteral("rx")
        || m == QStringLiteral("in"))
        return MetricPart::Rx;
    if (m == QStringLiteral("rate_out") || m == QStringLiteral("tx_rate")
        || m == QStringLiteral("rate_tx") || m == QStringLiteral("tx")
        || m == QStringLiteral("out"))
        return MetricPart::Tx;
    if (m == QStringLiteral("rate_total") || m == QStringLiteral("total_rate")
        || m == QStringLiteral("rate") || m == QStringLiteral("total"))
        return MetricPart::Total;
    return std::nullopt;
}

QString metricText(MetricPart part)
{
    switch (part) {
    case MetricPart::Rx:    return QStringLiteral("rx");
    case MetricPart::Tx:    return QStringLiteral("tx");
    case MetricPart::Total: return QStringLiteral("total");
    }
    return QStringLiteral("rate");
}

QString perfLabel(MetricPart part)
{
    return metricText(part) + QStringLiteral("_rate");
}

double metricValue(MetricPart part, double rx, double tx)
{
    switch (part) {
    case MetricPart::Rx:    return rx;
    case MetricPart::Tx:    return tx;
    case MetricPart::Total: return rx + tx;
    }
    return rx + tx;
}

QString perfNumber(double value)
{
    return QString::number(std::max(0.0, value), 'f', 0);
}

void printNagiosLine(int code, const QString &message, MetricPart part,
                     double value, double warning, double critical,
                     bool includePerfdata)
{
    QString state;
    switch (code) {
    case Ok:       state = QStringLiteral("OK"); break;
    case Warning:  state = QStringLiteral("WARNING"); break;
    case Critical: state = QStringLiteral("CRITICAL"); break;
    default:       state = QStringLiteral("UNKNOWN"); break;
    }

    QTextStream out(stdout);
    out << QStringLiteral("QIFTOP %1 - %2").arg(state, message);
    if (includePerfdata) {
        out << QStringLiteral(" | %1=%2B/s;%3;%4;0")
                   .arg(perfLabel(part),
                        perfNumber(value),
                        perfNumber(warning),
                        perfNumber(critical));
    }
    out << '\n';
    out.flush();
}

int statusFor(double value, double warning, double critical)
{
    if (value >= critical)
        return Critical;
    if (value >= warning)
        return Warning;
    return Ok;
}

QString argValue(const QStringList &args, int &i, QString *error)
{
    if (i + 1 >= args.size()) {
        *error = QStringLiteral("missing value for %1").arg(args.at(i));
        return {};
    }
    ++i;
    return args.at(i);
}

std::optional<Options> parseOptions(const QStringList &args, QString *error)
{
    Options opts;
    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QStringLiteral("--session")) {
            opts.sessionBus = true;
        } else if (arg == QStringLiteral("--metric")) {
            opts.metricArg = argValue(args, i, error);
            if (!error->isEmpty()) return std::nullopt;
            const auto metric = parseMetric(opts.metricArg);
            if (!metric) {
                *error = QStringLiteral("unknown metric '%1'").arg(opts.metricArg);
                return std::nullopt;
            }
            opts.part = *metric;
        } else if (arg == QStringLiteral("--iface")) {
            opts.iface = argValue(args, i, error).trimmed();
            if (!error->isEmpty()) return std::nullopt;
        } else if (arg == QStringLiteral("--filter")) {
            opts.filter = argValue(args, i, error);
            if (!error->isEmpty()) return std::nullopt;
        } else if (arg == QStringLiteral("--warning")) {
            const QString v = argValue(args, i, error);
            if (!error->isEmpty()) return std::nullopt;
            const auto parsed = parseByteRate(v);
            if (!parsed) {
                *error = QStringLiteral("invalid warning threshold '%1'").arg(v);
                return std::nullopt;
            }
            opts.warning = *parsed;
            opts.haveWarning = true;
        } else if (arg == QStringLiteral("--critical")) {
            const QString v = argValue(args, i, error);
            if (!error->isEmpty()) return std::nullopt;
            const auto parsed = parseByteRate(v);
            if (!parsed) {
                *error = QStringLiteral("invalid critical threshold '%1'").arg(v);
                return std::nullopt;
            }
            opts.critical = *parsed;
            opts.haveCritical = true;
        } else if (arg == QStringLiteral("--timeout")) {
            bool ok = false;
            const int v = argValue(args, i, error).toInt(&ok);
            if (!error->isEmpty()) return std::nullopt;
            if (!ok || v <= 0) {
                *error = QStringLiteral("invalid timeout");
                return std::nullopt;
            }
            opts.timeoutSec = v;
        } else if (arg == QStringLiteral("--interval-ms")) {
            bool ok = false;
            const int v = argValue(args, i, error).toInt(&ok);
            if (!error->isEmpty()) return std::nullopt;
            if (!ok || v <= 0) {
                *error = QStringLiteral("invalid interval");
                return std::nullopt;
            }
            opts.intervalMs = v;
        } else {
            *error = QStringLiteral("unknown argument '%1'").arg(arg);
            return std::nullopt;
        }
    }

    if (!opts.haveWarning || !opts.haveCritical) {
        *error = QStringLiteral("--warning and --critical are required");
        return std::nullopt;
    }
    if (opts.critical < opts.warning) {
        *error = QStringLiteral("--critical must be greater than or equal to --warning");
        return std::nullopt;
    }
    if (!opts.iface.isEmpty() && !opts.filter.isEmpty()) {
        *error = QStringLiteral("--iface and --filter are mutually exclusive; use --filter iface=<name> for flow checks");
        return std::nullopt;
    }
    return opts;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});

    QString parseError;
    const auto options = parseOptions(app.arguments(), &parseError);
    if (!options) {
        printNagiosLine(Unknown, parseError, MetricPart::Total, 0.0, 0.0, 0.0, false);
        return Unknown;
    }
    const Options &opts = *options;

    qRegisterMetaType<InterfaceStats>();
    qRegisterMetaType<QList<InterfaceStats>>();
    qRegisterMetaType<Connection>();
    qRegisterMetaType<QList<Connection>>();
    qiftop::dbus::registerTypes();

    if (opts.iface.isEmpty()) {
        const auto filter = qiftop::filter::parse(opts.filter);
        if (!filter.error.isEmpty()) {
            printNagiosLine(Unknown,
                            QStringLiteral("invalid filter at column %1: %2")
                                .arg(filter.errorPos + 1)
                                .arg(filter.error),
                            opts.part, 0.0, opts.warning, opts.critical, true);
            return Unknown;
        }

        auto *monitor = new DBusConnectionMonitor(opts.sessionBus, &app);
        auto *agg = new ConnectionAggregator(&app);
        agg->setPollIntervalMs(opts.intervalMs);
        QObject::connect(monitor, &ConnectionMonitor::connectionsUpdated,
                         agg, &ConnectionAggregator::updateConnections);

        int samples = 0;
        bool finished = false;
        const QString subject = opts.filter.isEmpty()
            ? QStringLiteral("flows")
            : QStringLiteral("flows[%1]").arg(opts.filter);
        const auto finish = [&](int code, const QString &message, double value) {
            if (finished) return;
            finished = true;
            printNagiosLine(code, message, opts.part, value, opts.warning, opts.critical, true);
            app.exit(code);
        };

        QObject::connect(monitor, &ConnectionMonitor::permissionDenied, &app,
                         [&](const QString &detail) {
            finish(Unknown, QStringLiteral("permission denied: %1").arg(detail), 0.0);
        });
        QObject::connect(monitor, &ConnectionMonitor::connectionsUpdated, &app, [&] {
            ++samples;
            if (samples < 2 || finished)
                return;

            double rx = 0.0;
            double tx = 0.0;
            for (const auto &row : agg->rows()) {
                qiftop::filter::Context ctx{row.current, row.rxRaw, row.txRaw, {}, {}};
                if (!qiftop::filter::matches(filter.expr, ctx))
                    continue;
                rx += row.rxRaw;
                tx += row.txRaw;
            }
            const double value = metricValue(opts.part, rx, tx);
            const int code = statusFor(value, opts.warning, opts.critical);
            finish(code,
                   QStringLiteral("%1 %2 %3").arg(subject, metricText(opts.part), util::formatByteRate(value)),
                   value);
        });

        QTimer::singleShot(opts.timeoutSec * 1000, &app, [&] {
            finish(Unknown, QStringLiteral("timed out waiting for qiftop-agent connection data"), 0.0);
        });

        monitor->setDesiredIntervalMs(opts.intervalMs);
        monitor->start();
        return app.exec();
    }

    auto *monitor = new DBusNetworkMonitor(opts.sessionBus, &app);
    auto *agg = new InterfaceAggregator(&app);
    QObject::connect(monitor, &NetworkMonitor::statsUpdated,
                     agg, &InterfaceAggregator::updateStats);

    int samples = 0;
    bool finished = false;
    const auto finish = [&](int code, const QString &message, double value) {
        if (finished) return;
        finished = true;
        printNagiosLine(code, message, opts.part, value, opts.warning, opts.critical, true);
        app.exit(code);
    };
    QObject::connect(monitor, &NetworkMonitor::statsUpdated, &app, [&] {
        ++samples;
        if (samples < 2 || finished)
            return;

        const auto &rows = agg->rows();
        const auto it = std::ranges::find_if(rows, [&](const InterfaceAggregator::Row &row) {
            return row.current.name == opts.iface;
        });
        if (it == rows.cend()) {
            finish(Unknown, QStringLiteral("interface '%1' not found").arg(opts.iface), 0.0);
            return;
        }

        const double value = metricValue(opts.part, it->rxRate, it->txRate);
        const int code = statusFor(value, opts.warning, opts.critical);
        finish(code,
               QStringLiteral("%1 %2 %3").arg(opts.iface, metricText(opts.part), util::formatByteRate(value)),
               value);
    });

    QTimer::singleShot(opts.timeoutSec * 1000, &app, [&] {
        finish(Unknown, QStringLiteral("timed out waiting for qiftop-agent interface data"), 0.0);
    });

    monitor->setDesiredIntervalMs(opts.intervalMs);
    monitor->start();
    return app.exec();
}
