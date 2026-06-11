#pragma once

#include <QAbstractTableModel>

#include "aggregate/InterfaceAggregator.h"
#include "backend/NetworkMonitor.h"
#include "util/Exportable.h"

class NetworkModel : public QAbstractTableModel, public Exportable {
    Q_OBJECT

public:
    enum class Column : int {
        Name,
        RxRate,
        TxRate,
        RxTotal,
        TxTotal,
        Status,
        ColumnCount,
    };

    // Custom roles for sort proxies / delegates that need raw numbers.
    enum Role {
        SortRole       = Qt::UserRole + 1,
        IsLoopbackRole,
        IsUpRole,
        DetailsRole,    // QString, e.g. "ethernet, MTU 1500, 192.168.1.10/24"
    };

    explicit NetworkModel(QObject *parent = nullptr);

    [[nodiscard]] int      rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] int      columnCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;

    // Exportable
    [[nodiscard]] QStringList  exportHeaders() const override;
    [[nodiscard]] int          exportRowCount() const override;
    [[nodiscard]] QVariantList exportRow(int row) const override;

public slots:
    void updateStats(QList<InterfaceStats> stats);

private:
    using Row = qiftop::aggregate::InterfaceAggregator::Row;

    // The model is a thin QAbstractItemModel adapter over the plain-QObject
    // aggregator that owns the rate computation + row set (shared with the
    // ncurses frontend). We translate the aggregator's coarse change signals
    // into begin/endResetModel() + dataChanged().
    qiftop::aggregate::InterfaceAggregator m_agg;
};
