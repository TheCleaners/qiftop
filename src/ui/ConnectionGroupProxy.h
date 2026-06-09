#pragma once

#include <QAbstractItemModel>
#include <QList>
#include <QObject>
#include <QPersistentModelIndex>
#include <QString>
#include <QVariant>

#include "config/Settings.h"

// Tree-of-groups model wrapping the flat Connections source (ultimately
// ConnectionModel via the filter proxy). Modes:
//
//   Flat       — pass-through: every source row becomes a top-level item
//                (parent = invalid). Visually identical to the v0.1
//                QTableView so the existing RowGaugeDelegate /
//                ConnectionFlowDelegate keep painting the way users expect.
//   ByInterface / ByContainer / ByProcess — tree of two levels:
//                group rows at depth 0 (aggregated rate / byte / packet
//                sums + child-flow count), individual flows at depth 1.
//
// Flat mode forwards source structure/data changes without resetting so
// selection, scroll anchors, and current index behave exactly as they did
// when the view was attached directly to the flat source model.
//
// Per-row data for child flows is forwarded verbatim to the source row.
// Group rows synthesize their own DisplayRole text ("kube-pod-X  [12]")
// in the Flow column and sum the rate / byte columns. SortRole on a
// group row returns its summed numeric value so descending sort by
// RxRate puts the busiest groups on top.
class ConnectionGroupProxy : public QAbstractItemModel {
    Q_OBJECT
public:
    using ViewMode = Settings::ConnectionViewMode;

    explicit ConnectionGroupProxy(QObject *parent = nullptr);

    void setSourceModel(QAbstractItemModel *src);
    [[nodiscard]] QAbstractItemModel *sourceModel() const { return m_src; }

    void setViewMode(ViewMode mode);
    [[nodiscard]] ViewMode viewMode() const { return m_mode; }

    // True for the synthetic group row at index. False for flat rows
    // and for child flow rows. Used by the view (delegate selection,
    // context menu).
    [[nodiscard]] bool isGroupIndex(const QModelIndex &idx) const;

    // Maps an index in THIS model to the underlying flat source index.
    // Returns an invalid index for group rows (they have no source).
    [[nodiscard]] QModelIndex mapToSource(const QModelIndex &proxy) const;
    [[nodiscard]] QModelIndex mapFromSource(const QModelIndex &src) const;

    // --- QAbstractItemModel ---
    [[nodiscard]] QModelIndex index(int row, int column,
                                    const QModelIndex &parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex &child) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;

private slots:
    void onSourceReset();
    void onSourceRowsAboutToBeInserted(const QModelIndex &parent, int first, int last);
    void onSourceRowsInserted(const QModelIndex &parent, int first, int last);
    void onSourceRowsAboutToBeRemoved(const QModelIndex &parent, int first, int last);
    void onSourceRowsRemoved(const QModelIndex &parent, int first, int last);
    void onSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                             const QVector<int> &roles);

private:
    struct Group {
        QString key;
        QString label;         // user-visible group title
        QList<int> srcRows;    // sorted ascending
    };

    void rebuild();
    [[nodiscard]] QString groupKeyFor(int srcRow) const;
    [[nodiscard]] QString groupLabelFor(int srcRow, const QString &key) const;
    [[nodiscard]] QVariant aggregateData(const Group &g, int column, int role) const;
    [[nodiscard]] int groupIndexForSourceRow(int srcRow) const;
    void forwardSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                  const QVector<int> &roles);

    QAbstractItemModel *m_src = nullptr;
    ViewMode m_mode = ViewMode::Flat;
    QList<Group> m_groups;     // empty when mode==Flat
};
