#include "ConnectionModel.h"
#include "util/Units.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

using qiftop::aggregate::ConnectionAggregator;

// How many samples a connection must accumulate before its adaptive
// reference (and therefore the gauge fraction / Max columns) is considered
// meaningful. The first 1–2 samples don't have enough history for the CMA
// to differ from the instantaneous reading, which would otherwise paint the
// gauge at ~100% on every new connection.
static constexpr quint64 kGaugeWarmupSamples = 4;

ConnectionModel::ConnectionModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    // Translate the aggregator's granular change signals 1:1 onto the
    // QAbstractItemModel protocol. The connections are direct (same thread),
    // so an *AboutTo* signal -> begin*, the aggregator mutates m_rows, then
    // the completion signal -> end*, bracketing the mutation exactly.
    const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
    connect(&m_agg, &ConnectionAggregator::rowsAboutToBeInserted, this,
            [this](int first, int last) { beginInsertRows({}, first, last); });
    connect(&m_agg, &ConnectionAggregator::rowsInserted, this,
            [this] { endInsertRows(); });
    connect(&m_agg, &ConnectionAggregator::rowsAboutToBeRemoved, this,
            [this](int first, int last) { beginRemoveRows({}, first, last); });
    connect(&m_agg, &ConnectionAggregator::rowsRemoved, this,
            [this] { endRemoveRows(); });
    connect(&m_agg, &ConnectionAggregator::rowsUpdated, this,
            [this, lastCol](int first, int last) {
                emit dataChanged(index(first, 0), index(last, lastCol));
            });
    connect(&m_agg, &ConnectionAggregator::viewDataChanged, this,
            [this, lastCol] {
                if (m_agg.rowCount() > 0)
                    emit dataChanged(index(0, 0), index(m_agg.rowCount() - 1, lastCol));
            });
}

int ConnectionModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_agg.rowCount();
}

int ConnectionModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Column::ColumnCount);
}

void ConnectionModel::setTintRowByDirection(bool v)
{
    if (v == m_tintRowByDirection) return;
    m_tintRowByDirection = v;
    if (m_agg.rowCount() > 0) {
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0),
                         index(m_agg.rowCount() - 1, lastCol),
                         {Qt::BackgroundRole});
    }
}

void ConnectionModel::setThroughputGaugeEnabled(bool v)
{
    if (v == m_gaugeEnabled) return;
    m_gaugeEnabled = v;
    if (m_agg.rowCount() > 0) {
        const int lastCol = static_cast<int>(Column::ColumnCount) - 1;
        emit dataChanged(index(0, 0), index(m_agg.rowCount() - 1, lastCol));
    }
}

QVariant ConnectionModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_agg.rowCount())
        return {};

    const Row        &row = m_agg.rowAt(index.row());
    const auto        col = static_cast<Column>(index.column());
    const Connection &c   = row.current;

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Column::Iface:   return c.iface.isEmpty() ? QStringLiteral("—") : c.iface;
        case Column::Flow:
            return QStringLiteral("%1  %2 → %3")
                .arg(m_agg.protoLabel(c), m_agg.endpointText(c.local), m_agg.endpointText(c.remote));
        case Column::RxRate:  return util::formatByteRate(row.rxRate);
        case Column::TxRate:  return util::formatByteRate(row.txRate);
        case Column::RxTotal: return util::formatBytes(c.rxBytes);
        case Column::TxTotal: return util::formatBytes(c.txBytes);
        case Column::RxMax: {
            if (row.samples < kGaugeWarmupSamples)
                return QStringLiteral("—");
            const double v = m_agg.rxReference(row);
            return v > 0.0 ? util::formatByteRate(v) : QStringLiteral("—");
        }
        case Column::TxMax: {
            if (row.samples < kGaugeWarmupSamples)
                return QStringLiteral("—");
            const double v = m_agg.txReference(row);
            return v > 0.0 ? util::formatByteRate(v) : QStringLiteral("—");
        }
        case Column::Process:
            if (c.process.pid > 0) {
                const QString name = c.process.comm.isEmpty()
                                         ? QStringLiteral("pid %1").arg(c.process.pid)
                                         : c.process.comm;
                return QStringLiteral("%1  [%2]").arg(name).arg(c.process.pid);
            }
            return QStringLiteral("—");
        case Column::Container: {
            const auto &ci = c.container;
            if (ci.runtime.isEmpty() && ci.name.isEmpty() && ci.id.isEmpty())
                return QStringLiteral("(host)");
            const QString display = !ci.name.isEmpty() ? ci.name : ci.id.left(12);
            const QString primary = ci.runtime.isEmpty()
                                        ? display
                                        : QStringLiteral("%1:%2").arg(ci.runtime, display);
            if (c.containerChain.size() >= 2)
                return QStringLiteral("%1  ▸").arg(primary);
            return primary;
        }
        case Column::ColumnCount: break;
        }
        return {};

    case SortRole:
        switch (col) {
        case Column::Iface:   return c.iface;
        case Column::Flow:    return c.remote.address.toString();
        case Column::RxRate:  return row.rxRate;
        case Column::TxRate:  return row.txRate;
        case Column::RxTotal: return static_cast<qulonglong>(c.rxBytes);
        case Column::TxTotal: return static_cast<qulonglong>(c.txBytes);
        case Column::RxMax:   return m_agg.rxReference(row);
        case Column::TxMax:   return m_agg.txReference(row);
        case Column::Process: return static_cast<qulonglong>(c.process.pid);
        case Column::Container: {
            const auto &ci = c.container;
            if (!ci.name.isEmpty()) return ci.name;
            if (!ci.id.isEmpty())   return ci.id;
            return QStringLiteral("\u0001(host)"); // sort to one end
        }
        case Column::ColumnCount: break;
        }
        return {};

    case IsIPv6Role:
        return c.local.isIPv6() || c.remote.isIPv6();
    case ProtoTextRole:
        return m_agg.protoLabel(c);
    case ProtoRole:
        return static_cast<int>(c.proto);
    case LocalTextRole:
        return m_agg.endpointText(c.local);
    case RemoteTextRole:
        return m_agg.endpointText(c.remote);
    case IsStaleRole:
        return row.stale;
    case IfaceNameRole:
        return c.iface;
    case DirectionRole:
        return static_cast<int>(c.direction);
    case ConnectionRole:
        return QVariant::fromValue(c);
    case RxRateRole:
        return row.rxRate;
    case TxRateRole:
        return row.txRate;
    case HostnameLocalRole:
        return m_agg.cachedHostname(c.local.address);
    case HostnameRemoteRole:
        return m_agg.cachedHostname(c.remote.address);
    case ProcessPidRole:
        return c.process.pid;
    case ProcessCommRole:
        return c.process.comm;
    case ContainerRuntimeRole:
        return c.container.runtime;
    case ContainerIdRole:
        return c.container.id;
    case ContainerNameRole:
        return c.container.name;
    case ContainerChainRole: {
        QStringList out;
        out.reserve(c.containerChain.size());
        for (const auto &ci : c.containerChain) {
            const QString display = !ci.name.isEmpty() ? ci.name : ci.id.left(12);
            out << (ci.runtime.isEmpty()
                        ? display
                        : QStringLiteral("%1:%2").arg(ci.runtime, display));
        }
        return out;
    }

    case Qt::ToolTipRole: {
        // Tooltip injection hardening (M2): Qt auto-detects rich text in
        // tooltips, and comm / container runtime/id/name are attacker-
        // controlled. Escape every dynamic field and render as DELIBERATE
        // rich text (<qt> + <br>).
        const auto esc = [](const QString &s) { return s.toHtmlEscaped(); };
        switch (col) {
        case Column::Process:
            if (c.process.pid <= 0) return QStringLiteral("Unattributed flow");
            return QStringLiteral("<qt>pid: %1<br>comm: %2<br>uid: %3<br><br>Right-click → Details for cmdline / exe / cwd</qt>")
                .arg(c.process.pid)
                .arg(c.process.comm.isEmpty() ? QStringLiteral("?")
                                              : esc(c.process.comm))
                .arg(c.process.uid);
        case Column::Container: {
            if (c.container.runtime.isEmpty() && c.container.id.isEmpty()
                && c.container.name.isEmpty()) {
                return QStringLiteral("Host process (no container)");
            }
            QString s = QStringLiteral("runtime: %1<br>id: %2<br>name: %3")
                .arg(c.container.runtime.isEmpty() ? QStringLiteral("?") : esc(c.container.runtime),
                     c.container.id.isEmpty()      ? QStringLiteral("?") : esc(c.container.id),
                     c.container.name.isEmpty()    ? QStringLiteral("?") : esc(c.container.name));
            if (c.containerChain.size() >= 2 && m_showContainerChainInTooltip) {
                s += QStringLiteral("<br><br>Nesting (outer → inner):");
                for (const auto &ci : c.containerChain) {
                    const QString disp = !ci.name.isEmpty() ? ci.name
                                                            : ci.id.left(12);
                    s += QStringLiteral("<br>&nbsp;&nbsp;• %1:%2")
                             .arg(ci.runtime.isEmpty() ? QStringLiteral("?") : esc(ci.runtime),
                                  esc(disp));
                }
            }
            return QStringLiteral("<qt>%1</qt>").arg(s);
        }
        default: return {};
        }
    }

    case GaugeFractionRole: {
        if (!m_gaugeEnabled)
            return {};
        if (row.samples < kGaugeWarmupSamples)
            return 0.0;
        const double refRx = m_agg.rxReference(row);
        const double refTx = m_agg.txReference(row);
        const double ref   = refRx + refTx;
        if (ref <= 0.0)
            return 0.0;
        const double cur = row.rxRate + row.txRate;
        return qBound(0.0, cur / ref, 1.0);
    }
    case GaugeDarkColorRole: {
        if (!m_gaugeEnabled)
            return {};
        const QColor base = QApplication::palette().color(QPalette::Base);
        const bool   dark = base.lightness() < 128;
        if (m_tintRowByDirection) {
            if (m_agg.isForwardedFlow(c))
                return dark ? QColor(130, 110, 40) : QColor(235, 215, 130);
            if (c.direction == Direction::Outbound)
                return dark ? QColor(60, 130, 60) : QColor(170, 220, 170);
            if (c.direction == Direction::Inbound)
                return dark ? QColor(140, 60, 60) : QColor(240, 180, 180);
        }
        return dark ? QColor(90, 90, 90) : QColor(190, 190, 190);
    }

    case Qt::FontRole: {
        QFont f;
        if (row.stale) f.setItalic(true);
        return f;
    }
    case Qt::ForegroundRole:
        if (row.stale)
            return QVariant::fromValue(QApplication::palette().color(QPalette::Disabled, QPalette::Text));
        return {};

    case Qt::BackgroundRole:
        if (m_tintRowByDirection) {
            const QColor baseCol = QApplication::palette().color(QPalette::Base);
            const bool   dark    = baseCol.lightness() < 128;
            if (m_agg.isForwardedFlow(c)) {
                if (m_gaugeEnabled)
                    return dark ? QColor(45, 40, 22) : QColor(252, 248, 230);
                return dark ? QColor(60, 52, 28) : QColor(248, 240, 200);
            }
            if (c.direction == Direction::Outbound) {
                if (m_gaugeEnabled)
                    return dark ? QColor(28, 50, 28) : QColor(235, 250, 235);
                return dark ? QColor(36, 70, 36) : QColor(220, 245, 220);
            }
            if (c.direction == Direction::Inbound) {
                if (m_gaugeEnabled)
                    return dark ? QColor(50, 28, 28) : QColor(252, 235, 235);
                return dark ? QColor(70, 36, 36) : QColor(250, 220, 220);
            }
        }
        return {};

    case Qt::TextAlignmentRole:
        if (col == Column::Flow || col == Column::Iface)
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    }
    return {};
}

QVariant ConnectionModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (static_cast<Column>(section)) {
    case Column::Iface:   return tr("Iface");
    case Column::Flow:    return tr("Flow");
    case Column::RxRate:  return tr("RX rate");
    case Column::TxRate:  return tr("TX rate");
    case Column::RxTotal: return tr("RX total");
    case Column::TxTotal: return tr("TX total");
    case Column::RxMax:   return tr("Max RX");
    case Column::TxMax:   return tr("Max TX");
    case Column::Process: return tr("Process");
    case Column::Container: return tr("Container");
    case Column::ColumnCount: break;
    }
    return {};
}

QStringList ConnectionModel::exportHeaders() const
{
    return {
        QStringLiteral("iface"),
        QStringLiteral("proto"),
        QStringLiteral("localAddress"),
        QStringLiteral("localPort"),
        QStringLiteral("remoteAddress"),
        QStringLiteral("remotePort"),
        QStringLiteral("remoteHostname"),
        QStringLiteral("rxBytes"),
        QStringLiteral("txBytes"),
        QStringLiteral("rxPackets"),
        QStringLiteral("txPackets"),
        QStringLiteral("rxBytesPerSec"),
        QStringLiteral("txBytesPerSec"),
        QStringLiteral("rxBytesPerSecRef"),
        QStringLiteral("txBytesPerSecRef"),
        QStringLiteral("pid"),
        QStringLiteral("uid"),
        QStringLiteral("comm"),
        QStringLiteral("containerRuntime"),
        QStringLiteral("containerId"),
        QStringLiteral("containerName"),
    };
}

int ConnectionModel::exportRowCount() const
{
    return m_agg.rowCount();
}

QVariantList ConnectionModel::exportRow(int row) const
{
    if (row < 0 || row >= m_agg.rowCount())
        return {};
    const Row        &r = m_agg.rowAt(row);
    const Connection &c = r.current;
    const QString remoteName = m_agg.cachedHostname(c.remote.address);
    return {
        c.iface,
        l4ProtoToString(c.proto),
        c.local.address.toString(),
        c.local.port,
        c.remote.address.toString(),
        c.remote.port,
        remoteName,
        static_cast<qulonglong>(c.rxBytes),
        static_cast<qulonglong>(c.txBytes),
        static_cast<qulonglong>(c.rxPackets),
        static_cast<qulonglong>(c.txPackets),
        r.rxRate,
        r.txRate,
        m_agg.rxReference(r),
        m_agg.txReference(r),
        c.process.pid,
        c.process.uid,
        c.process.comm,
        c.container.runtime,
        c.container.id,
        c.container.name,
    };
}
