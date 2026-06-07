#include "NetworkModel.h"
#include "util/Units.h"

#include <algorithm>

NetworkModel::NetworkModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_elapsed.start();
}

int NetworkModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int NetworkModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Column::ColumnCount);
}

QVariant NetworkModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};

    const Row &row = m_rows[index.row()];
    const auto col = static_cast<Column>(index.column());

    switch (role) {
    case Qt::DisplayRole:
        switch (col) {
        case Column::Name:    return row.current.name;
        case Column::RxRate:  return util::formatByteRate(row.rxRate);
        case Column::TxRate:  return util::formatByteRate(row.txRate);
        case Column::RxTotal: return util::formatBytes(row.current.rxBytes);
        case Column::TxTotal: return util::formatBytes(row.current.txBytes);
        case Column::Status:
            if (row.current.isLoopback) return QStringLiteral("loopback");
            return row.current.isUp ? QStringLiteral("up") : QStringLiteral("down");
        case Column::ColumnCount: break;
        }
        return {};

    case SortRole:
        switch (col) {
        case Column::Name:    return row.current.name;
        case Column::RxRate:  return row.rxRate;
        case Column::TxRate:  return row.txRate;
        case Column::RxTotal: return static_cast<qulonglong>(row.current.rxBytes);
        case Column::TxTotal: return static_cast<qulonglong>(row.current.txBytes);
        case Column::Status:  return row.current.isUp ? 1 : 0;
        case Column::ColumnCount: break;
        }
        return {};

    case Qt::TextAlignmentRole:
        if (col == Column::Name || col == Column::Status)
            return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);

    case IsLoopbackRole:
        return row.current.isLoopback;
    case IsUpRole:
        return row.current.isUp;
    case DetailsRole: {
        QStringList parts;
        if (!row.current.type.isEmpty())
            parts << row.current.type;
        if (row.current.mtu > 0)
            parts << QStringLiteral("MTU %1").arg(row.current.mtu);
        if (!row.current.addresses.isEmpty())
            parts << row.current.addresses.join(QStringLiteral(", "));
        return parts.join(QStringLiteral(", "));
    }
    }

    return {};
}

QVariant NetworkModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};

    switch (static_cast<Column>(section)) {
    case Column::Name:    return tr("Interface");
    case Column::RxRate:  return tr("RX rate");
    case Column::TxRate:  return tr("TX rate");
    case Column::RxTotal: return tr("RX total");
    case Column::TxTotal: return tr("TX total");
    case Column::Status:  return tr("Status");
    case Column::ColumnCount: break;
    }
    return {};
}

QStringList NetworkModel::exportHeaders() const
{
    return {
        QStringLiteral("name"),
        QStringLiteral("type"),
        QStringLiteral("mtu"),
        QStringLiteral("addresses"),
        QStringLiteral("isUp"),
        QStringLiteral("isLoopback"),
        QStringLiteral("rxBytes"),
        QStringLiteral("txBytes"),
        QStringLiteral("rxPackets"),
        QStringLiteral("txPackets"),
        QStringLiteral("rxBytesPerSec"),
        QStringLiteral("txBytesPerSec"),
    };
}

int NetworkModel::exportRowCount() const
{
    return static_cast<int>(m_rows.size());
}

QVariantList NetworkModel::exportRow(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row &r = m_rows[row];
    const InterfaceStats &s = r.current;
    return {
        s.name,
        s.type,
        s.mtu,
        QVariant::fromValue(s.addresses),
        s.isUp,
        s.isLoopback,
        static_cast<qulonglong>(s.rxBytes),
        static_cast<qulonglong>(s.txBytes),
        static_cast<qulonglong>(s.rxPackets),
        static_cast<qulonglong>(s.txPackets),
        r.rxRate,
        r.txRate,
    };
}

void NetworkModel::updateStats(QList<InterfaceStats> stats)
{
    const qint64 nowMs     = m_elapsed.elapsed();
    const qint64 deltaMs   = nowMs - m_lastElapsedMs;
    m_lastElapsedMs        = nowMs;
    const double deltaSecs = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // Sort by name for deterministic row order; QSortFilterProxyModel can re-sort.
    std::ranges::sort(stats, {}, &InterfaceStats::name);

    // Decide whether the row set itself changed (add/remove). Resetting the model is
    // simplest and correct; interface churn is rare so the cost is negligible.
    const bool structureChanged = [&] {
        if (stats.size() != m_rows.size())
            return true;
        for (qsizetype i = 0; i < stats.size(); ++i) {
            if (stats[i].name != m_rows[i].current.name)
                return true;
        }
        return false;
    }();

    if (structureChanged) {
        beginResetModel();
        m_rows.clear();
        m_rows.reserve(stats.size());
        for (const InterfaceStats &s : stats) {
            Row row;
            row.current = s;
            if (auto it = m_prev.constFind(s.name); it != m_prev.constEnd()) {
                row.rxRate = static_cast<double>(s.rxBytes - it->rxBytes) / deltaSecs;
                row.txRate = static_cast<double>(s.txBytes - it->txBytes) / deltaSecs;
            }
            m_rows.append(std::move(row));
        }
        endResetModel();
    } else {
        for (qsizetype i = 0; i < stats.size(); ++i) {
            const InterfaceStats &s = stats[i];
            Row &row = m_rows[i];
            if (auto it = m_prev.constFind(s.name); it != m_prev.constEnd()) {
                row.rxRate = static_cast<double>(s.rxBytes - it->rxBytes) / deltaSecs;
                row.txRate = static_cast<double>(s.txBytes - it->txBytes) / deltaSecs;
            }
            row.current = s;
        }
        emit dataChanged(index(0, 0),
                         index(static_cast<int>(m_rows.size()) - 1,
                               static_cast<int>(Column::ColumnCount) - 1));
    }

    // Refresh previous-sample cache
    m_prev.clear();
    m_prev.reserve(stats.size());
    for (const InterfaceStats &s : stats)
        m_prev.insert(s.name, s);
}
