#pragma once

#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>

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
    struct Row {
        InterfaceStats current{};
        double rxRate = 0.0; // bytes/sec
        double txRate = 0.0; // bytes/sec
    };

    QList<Row>                     m_rows; // sorted by name
    QHash<QString, InterfaceStats> m_prev;
    QElapsedTimer                  m_elapsed;
    qint64                         m_lastElapsedMs = 0;
};
