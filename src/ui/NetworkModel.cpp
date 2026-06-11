#include "NetworkModel.h"
#include "util/Units.h"

NetworkModel::NetworkModel(QObject *parent)
    : QAbstractTableModel(parent)
{
    // Translate the aggregator's model-agnostic change signals into the
    // QAbstractItemModel protocol. beginResetModel() must run BEFORE the
    // aggregator mutates its rows, so it's wired to aboutToReset().
    connect(&m_agg, &qiftop::aggregate::InterfaceAggregator::aboutToReset,
            this, [this] { beginResetModel(); });
    connect(&m_agg, &qiftop::aggregate::InterfaceAggregator::didReset,
            this, [this] { endResetModel(); });
    connect(&m_agg, &qiftop::aggregate::InterfaceAggregator::rowsChanged,
            this, [this](int first, int last) {
                emit dataChanged(index(first, 0),
                                 index(last, static_cast<int>(Column::ColumnCount) - 1));
            });
}

int NetworkModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_agg.rowCount();
}

int NetworkModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(Column::ColumnCount);
}

QVariant NetworkModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_agg.rowCount())
        return {};

    const Row &row = m_agg.rowAt(index.row());
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
    return m_agg.rowCount();
}

QVariantList NetworkModel::exportRow(int row) const
{
    if (row < 0 || row >= m_agg.rowCount())
        return {};
    const Row &r = m_agg.rowAt(row);
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
    m_agg.updateStats(std::move(stats));
}

