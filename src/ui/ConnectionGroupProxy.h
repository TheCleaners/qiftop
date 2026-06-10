#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QList>
#include <QPair>
#include <QObject>
#include <QPersistentModelIndex>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include "config/Settings.h"
#include "backend/ProcessDetails.h"

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

    // When true, group header rows carry extra attribution detail
    // inline (pid / user for ByProcess, full container id for
    // ByContainer) in the Flow column, plus a full multi-line
    // ToolTipRole. Gated by Settings::showGroupHeaderDetails. Triggers
    // a repaint of existing group rows when toggled.
    void setShowGroupDetails(bool on);
    [[nodiscard]] bool showGroupDetails() const { return m_showGroupDetails; }

    // On-demand process-detail enrichment (exe / cmdline / cwd). The
    // cache is owned by MainWindow and populated asynchronously from the
    // agent's GetProcessDetails RPC; the group-header tooltip reads from
    // it when present. Pointer (not copy) so updates are seen without
    // re-plumbing. May be null (in-process backend / details disabled).
    void setProcessDetailsCache(const QHash<qint32, qiftop::backend::ProcessDetails> *cache)
    { m_details = cache; }

    // Representative pid of a ByProcess group row (the pid all its flows
    // share). 0 for non-group indices, non-ByProcess modes, or the
    // "(unattributed)" bucket. Used by MainWindow to prefetch details.
    [[nodiscard]] qint32 representativePid(const QModelIndex &groupIdx) const;

    // Re-emit ToolTipRole dataChanged for every group row so a freshly
    // arrived detail shows on the next hover. Cheap (group count is
    // small).
    void refreshGroupTooltips();

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

    // Sort entry point invoked by the view (header click,
    // sortByColumn during setSortingEnabled/state restore). Both modes
    // remember the column/order so rebuild() re-applies it after a
    // group restructure.
    //
    //   Flat mode:    forwarded to source (QSortFilterProxyModel does
    //                 the work and emits layoutChanged; we re-emit in
    //                 the connect lambda).
    //   Grouped mode: sorts m_groups by aggregated SortRole value and
    //                 each group's srcRows by source SortRole value,
    //                 then emits layoutChanged with full persistent
    //                 index remapping so selection and expansion state
    //                 survive sort clicks.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

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
    // Compact inline detail appended to a group header's Flow column,
    // and the full multi-line tooltip — both derived from a
    // representative flow of the group (all flows in a Process/Container
    // group share the same attribution). Empty for ByInterface / when
    // the group has no attribution.
    [[nodiscard]] QString groupDetailInline(const Group &g) const;
    [[nodiscard]] QString groupDetailTooltip(const Group &g) const;
    // Colour-codable group-header segments (GroupChipsRole): a list of
    // {"text", "kind"} maps the ConnectionFlowDelegate paints with
    // theme-aware accents. kind ∈ {process, pid, user, cmdline,
    // container, id, iface, count}.
    [[nodiscard]] QVariantList groupChips(const Group &g) const;
    [[nodiscard]] int groupIndexForSourceRow(int srcRow) const;
    void forwardSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                  const QVector<int> &roles);

    // Surgically move a single source row to the group its CURRENT
    // group key maps to, emitting fine-grained begin/endRemoveRows +
    // begin/endInsertRows (creating / destroying groups as needed)
    // instead of a wholesale model reset. Used when a flow's group key
    // changes mid-stream — most often in ByContainer/ByProcess modes
    // when attribution resolves a flow from "(unattributed)" to a real
    // pid/container a tick or two after it first appears. A reset here
    // would collapse every expanded group in the view; the surgical
    // move preserves expansion + selection. Returns true if a
    // STRUCTURAL change happened (row moved groups), false if only the
    // group's display label was refreshed in place.
    bool regroupSourceRow(int srcRow);

    // Re-applies the current m_sortColumn / m_sortOrder to m_groups
    // and each group's srcRows. Cheap; called from rebuild() so that
    // group restructures after dataChanged keep the user's sort order
    // instead of snapping back to insertion order.
    void applyCurrentSort();

    // Sorts `groups` (and each group's srcRows) by the current
    // m_sortColumn / m_sortOrder. Shared by applyCurrentSort() and the
    // dry-run inside resortGroupsPreservingIndexes().
    void sortGroups(QList<Group> &groups) const;

    // Re-applies the current sort with the FULL Qt layout-change
    // protocol (layoutAboutToBeChanged → changePersistentIndexList →
    // layoutChanged) so outstanding QPersistentModelIndexes — the
    // view's expansion state, selection, current index — follow their
    // groups instead of keeping stale (row, internalId) coordinates.
    // No-op (no signals) when the order is already correct. Returns
    // true if a re-order happened. Must be called after any
    // insert/regroup/remove that can change group order; rebuild()
    // keeps using the signal-less applyCurrentSort() because it runs
    // inside a model reset.
    bool resortGroupsPreservingIndexes();

    // Rebuilds m_srcIndex from m_groups in one O(N) pass. Called after
    // every STRUCTURAL change (rebuild / sort / incremental insert /
    // incremental remove) — i.e. O(1) times per polling tick — so that
    // the per-row hot paths (mapFromSource, groupIndexForSourceRow,
    // forwardSourceDataChanged) become O(1) hash lookups instead of
    // O(N) linear scans across every group's srcRows. On a saturated
    // router (4096 flows) the old per-tick cost was O(rows²); now it's
    // O(rows). No-op in Flat mode (m_srcIndex stays empty there —
    // Flat's mapFromSource has its own direct 1:1 branch).
    void refreshSrcIndex();

    QAbstractItemModel *m_src = nullptr;
    ViewMode m_mode = ViewMode::Flat;
    QList<Group> m_groups;     // empty when mode==Flat
    bool m_showGroupDetails = true;
    const QHash<qint32, qiftop::backend::ProcessDetails> *m_details = nullptr;
    // Reverse index: source row → (group index, child row in group).
    // Empty in Flat mode. Rebuilt en masse by refreshSrcIndex().
    QHash<int, QPair<int, int>> m_srcIndex;
    int m_sortColumn = -1;     // -1 = no sort applied yet
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    // True while the Flat branch of sort() drives m_src->sort(): the
    // manual layout signals + persistent-index remap in sort() are the
    // single source of truth there, so the source-layout forwarding
    // lambdas must NOT also re-emit (double layoutAboutToBeChanged /
    // layoutChanged pairs corrupt the view's state save/restore).
    bool m_inFlatSort = false;
    // Snapshot of the pre-removal source-row span captured in
    // onSourceRowsAboutToBeRemoved so onSourceRowsRemoved can map
    // them back to (group, childRow) after the source has shifted.
    int m_pendingRemovalFirst = -1;
    int m_pendingRemovalLast  = -1;
};
