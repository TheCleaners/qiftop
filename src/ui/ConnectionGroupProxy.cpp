#include "ConnectionGroupProxy.h"

#include "ui/ConnectionModel.h"
#include "backend/Connection.h"
#include "backend/PlatformInfo.h"
#include "util/Units.h"

#include <QFont>
#include <QHash>
#include <QPartialOrdering>
#include <QSet>
#include <QSignalBlocker>
#include <QStringList>

#include <algorithm>

namespace {

constexpr quintptr kTopLevelId = static_cast<quintptr>(-1);

// Pull the typed Connection out of the source row via ConnectionRole.
// Returns a default-constructed Connection when the source isn't a
// ConnectionModel (or the role isn't present).
Connection connectionAt(QAbstractItemModel *src, int row)
{
    if (!src) return {};
    const QModelIndex idx = src->index(row, 0);
    return idx.data(ConnectionModel::ConnectionRole).value<Connection>();
}

} // namespace

ConnectionGroupProxy::ConnectionGroupProxy(QObject *parent)
    : QAbstractItemModel(parent)
{}

void ConnectionGroupProxy::setSourceModel(QAbstractItemModel *src)
{
    if (m_src == src) return;
    if (m_src) disconnect(m_src, nullptr, this, nullptr);
    beginResetModel();
    m_src = src;
    if (m_src) {
        connect(m_src, &QAbstractItemModel::modelReset,
                this,  &ConnectionGroupProxy::onSourceReset);
        connect(m_src, &QAbstractItemModel::rowsAboutToBeInserted,
                this, &ConnectionGroupProxy::onSourceRowsAboutToBeInserted);
        connect(m_src, &QAbstractItemModel::rowsInserted,
                this, &ConnectionGroupProxy::onSourceRowsInserted);
        connect(m_src, &QAbstractItemModel::rowsAboutToBeRemoved,
                this, &ConnectionGroupProxy::onSourceRowsAboutToBeRemoved);
        connect(m_src, &QAbstractItemModel::rowsRemoved,
                this, &ConnectionGroupProxy::onSourceRowsRemoved);
        connect(m_src, &QAbstractItemModel::layoutAboutToBeChanged, this,
                [this](const QList<QPersistentModelIndex> &parents,
                       QAbstractItemModel::LayoutChangeHint hint) {
                    if (m_mode == ViewMode::Flat)
                        emit layoutAboutToBeChanged(parents, hint);
                });
        connect(m_src, &QAbstractItemModel::layoutChanged, this,
                [this](const QList<QPersistentModelIndex> &parents,
                       QAbstractItemModel::LayoutChangeHint hint) {
                    if (m_mode == ViewMode::Flat) {
                        emit layoutChanged(parents, hint);
                    } else {
                        onSourceReset();
                    }
                });
        connect(m_src, &QAbstractItemModel::dataChanged, this,
                &ConnectionGroupProxy::onSourceDataChanged);
    }
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::setViewMode(ViewMode mode)
{
    if (mode == m_mode) return;
    beginResetModel();
    m_mode = mode;
    // Sorting ownership moves with the mode:
    //   Flat    — the group proxy is a pass-through, so the SOURCE
    //             (filter proxy) does the sorting; restore its sort.
    //   grouped — the group proxy sorts its own m_groups, and the
    //             source MUST NOT sort: a QSortFilterProxyModel with
    //             dynamicSortFilter re-sorts on every source dataChanged
    //             (rates change each tick), emitting layoutChanged every
    //             tick. In grouped mode that layoutChanged maps to a
    //             full onSourceReset() → the QTreeView collapses every
    //             tick. Clearing the source sort column eliminates the
    //             per-tick layoutChanged entirely.
    if (m_src) {
        // Block m_src signals around the sort() — sort() emits
        // layoutChanged synchronously, which our handler would otherwise
        // turn into a nested onSourceReset() mid-beginResetModel. We
        // rebuild() unconditionally right after, so the blocked signal
        // would be redundant anyway.
        const QSignalBlocker block(m_src);
        if (mode == ViewMode::Flat)
            m_src->sort(m_sortColumn, m_sortOrder);
        else
            m_src->sort(-1);
    }
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::onSourceReset()
{
    beginResetModel();
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::onSourceRowsAboutToBeInserted(const QModelIndex &parent, int first, int last)
{
    if (m_mode == ViewMode::Flat)
        beginInsertRows(mapFromSource(parent), first, last);
    // Grouped mode: we can't compute groupKeyFor() until the source
    // has actually been inserted (the data isn't readable yet). Defer
    // all begin/endInsertRows pairs to onSourceRowsInserted.
}

void ConnectionGroupProxy::onSourceRowsInserted(const QModelIndex &, int first, int last)
{
    if (m_mode == ViewMode::Flat) {
        endInsertRows();
        return;
    }
    // Grouped: process each inserted source row incrementally so the
    // view's expansion state, selection, and scroll position survive.
    // Pre-fix this called onSourceReset() — every newly tracked flow
    // collapsed the entire tree, which on a busy host means the user
    // can never keep a group expanded.
    const int insertedCount = last - first + 1;

    // 1. Shift existing srcRows >= first by the insertion count so
    //    they continue to point at the same logical source rows.
    for (Group &g : m_groups) {
        for (int &r : g.srcRows)
            if (r >= first) r += insertedCount;
    }

    // 2. For each newly inserted source row, classify it into a
    //    group and emit a single begin/endInsertRows pair (either
    //    a new top-level group row, or a child of an existing group).
    for (int srcRow = first; srcRow <= last; ++srcRow) {
        const QString key = groupKeyFor(srcRow);

        // Find existing group by key (linear scan; #groups is small —
        // an interface count or container count, not a flow count).
        int existing = -1;
        for (int gi = 0; gi < m_groups.size(); ++gi) {
            if (m_groups[gi].key == key) { existing = gi; break; }
        }

        if (existing >= 0) {
            // Append to existing group. srcRows stays sorted by row
            // arrival order (matches what rebuild() would produce).
            const QModelIndex parent = createIndex(existing, 0, kTopLevelId);
            const int childRow = m_groups[existing].srcRows.size();
            beginInsertRows(parent, childRow, childRow);
            m_groups[existing].srcRows.append(srcRow);
            endInsertRows();
        } else {
            // New group at the end of m_groups.
            const int groupRow = m_groups.size();
            beginInsertRows({}, groupRow, groupRow);
            Group g;
            g.key = key;
            g.label = groupLabelFor(srcRow, key);
            g.srcRows.append(srcRow);
            m_groups.append(std::move(g));
            endInsertRows();
        }
    }

    // 3. Re-apply current sort. applyCurrentSort emits no signals so
    //    this re-orders in place; the view will pick up the new order
    //    on its next paint. Skipped when no sort has been requested
    //    yet. Then rebuild the O(1) reverse index for the new layout.
    applyCurrentSort();
    refreshSrcIndex();
}

void ConnectionGroupProxy::onSourceRowsAboutToBeRemoved(const QModelIndex &parent, int first, int last)
{
    if (m_mode == ViewMode::Flat) {
        beginRemoveRows(mapFromSource(parent), first, last);
        return;
    }
    // Grouped: stash the about-to-be-removed source row span so
    // onSourceRowsRemoved can map them back to (group, childRow)
    // even after the source has shifted rows down. The source
    // model is still in pre-removal state RIGHT NOW so we MUST do
    // the lookup before the removal completes — but we MUST NOT emit
    // beginRemoveRows here, since Qt requires begin/end pairs to
    // bracket the SHORT moment the model is in an inconsistent
    // state, and we have nothing inconsistent until the source
    // actually removes the rows.
    m_pendingRemovalFirst = first;
    m_pendingRemovalLast  = last;
}

void ConnectionGroupProxy::onSourceRowsRemoved(const QModelIndex &, int first, int last)
{
    if (m_mode == ViewMode::Flat) {
        endRemoveRows();
        return;
    }
    // Use the snapshot if it matches; the args we just got should
    // equal the pre-removal first/last. Defensive in case of races.
    if (m_pendingRemovalFirst != first || m_pendingRemovalLast != last) {
        m_pendingRemovalFirst = first;
        m_pendingRemovalLast  = last;
    }
    const int removedCount = last - first + 1;

    // 1. For each removed source row, locate (group, childRow) and
    //    emit a begin/endRemoveRows pair. Track which groups became
    //    empty so we can remove them from the top level after.
    QList<int> emptiedGroups;
    for (int srcRow = first; srcRow <= last; ++srcRow) {
        for (int gi = 0; gi < m_groups.size(); ++gi) {
            const int childRow = static_cast<int>(
                m_groups[gi].srcRows.indexOf(srcRow));
            if (childRow < 0) continue;
            const QModelIndex parent = createIndex(gi, 0, kTopLevelId);
            beginRemoveRows(parent, childRow, childRow);
            m_groups[gi].srcRows.removeAt(childRow);
            endRemoveRows();
            if (m_groups[gi].srcRows.isEmpty() && !emptiedGroups.contains(gi))
                emptiedGroups.append(gi);
            break;  // a srcRow lives in exactly one group
        }
    }

    // 2. Shift remaining srcRows > last DOWN by removedCount so the
    //    proxy's view of source row numbers stays consistent.
    for (Group &g : m_groups) {
        for (int &r : g.srcRows)
            if (r > last) r -= removedCount;
    }

    // 3. Remove emptied groups. Sort descending so removal indices
    //    stay valid as we shrink m_groups.
    std::sort(emptiedGroups.begin(), emptiedGroups.end(), std::greater<int>());
    for (int gi : emptiedGroups) {
        beginRemoveRows({}, gi, gi);
        m_groups.removeAt(gi);
        endRemoveRows();
    }

    // The childRow positions within touched groups and the group
    // indices after any pruning have shifted — rebuild the O(1) index.
    refreshSrcIndex();

    m_pendingRemovalFirst = -1;
    m_pendingRemovalLast  = -1;
}

void ConnectionGroupProxy::onSourceDataChanged(const QModelIndex &topLeft,
                                               const QModelIndex &bottomRight,
                                               const QVector<int> &roles)
{
    if (m_mode == ViewMode::Flat) {
        emit dataChanged(index(topLeft.row(), topLeft.column()),
                         index(bottomRight.row(), bottomRight.column()),
                         roles);
        return;
    }

    const bool maybeGroupKeyChanged =
        roles.isEmpty() || roles.contains(ConnectionModel::ConnectionRole);
    if (maybeGroupKeyChanged) {
        // A flow's group key can change while it's live — most often
        // when attribution resolves it from "(unattributed)" to a real
        // pid/container a tick after it first appeared (ByProcess /
        // ByContainer). Move just that row to its correct group with
        // fine-grained insert/remove signals; the previous wholesale
        // onSourceReset() collapsed every expanded group in the view
        // every few seconds on a busy host (user-reported regression).
        for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
            regroupSourceRow(row);
    }

    forwardSourceDataChanged(topLeft, bottomRight, roles);
}

bool ConnectionGroupProxy::regroupSourceRow(int srcRow)
{
    const QString newKey = groupKeyFor(srcRow);
    const auto it = m_srcIndex.constFind(srcRow);
    const int curGi = (it != m_srcIndex.constEnd()) ? it->first : -1;

    if (curGi >= 0 && curGi < m_groups.size()
        && m_groups[curGi].key == newKey) {
        // Key unchanged — only the display label might have drifted
        // (e.g. comm resolved after the pid). Update it in place and
        // repaint the group header row; no structural change.
        const QString newLabel = groupLabelFor(srcRow, newKey);
        if (m_groups[curGi].label != newLabel) {
            m_groups[curGi].label = newLabel;
            const int lastCol = columnCount() - 1;
            emit dataChanged(index(curGi, 0), index(curGi, lastCol));
        }
        return false;
    }

    // --- structural move: remove from the current group, insert into
    //     the target group (creating it if it doesn't exist yet). ---
    if (curGi >= 0 && curGi < m_groups.size()) {
        const int childRow = it->second;
        beginRemoveRows(createIndex(curGi, 0, kTopLevelId), childRow, childRow);
        m_groups[curGi].srcRows.removeAt(childRow);
        endRemoveRows();
        if (m_groups[curGi].srcRows.isEmpty()) {
            beginRemoveRows({}, curGi, curGi);
            m_groups.removeAt(curGi);
            endRemoveRows();
        }
    }

    // Find the target group by key (group count is small — an iface /
    // container / process count, not a flow count).
    int targetGi = -1;
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        if (m_groups[gi].key == newKey) { targetGi = gi; break; }
    }
    if (targetGi >= 0) {
        const int childRow = m_groups[targetGi].srcRows.size();
        beginInsertRows(createIndex(targetGi, 0, kTopLevelId), childRow, childRow);
        m_groups[targetGi].srcRows.append(srcRow);
        endInsertRows();
    } else {
        const int groupRow = m_groups.size();
        beginInsertRows({}, groupRow, groupRow);
        Group g;
        g.key   = newKey;
        g.label = groupLabelFor(srcRow, newKey);
        g.srcRows.append(srcRow);
        m_groups.append(std::move(g));
        endInsertRows();
    }

    // Child positions + group indices shifted — rebuild the O(1) index.
    refreshSrcIndex();
    return true;
}

void ConnectionGroupProxy::rebuild()
{
    m_groups.clear();
    m_srcIndex.clear();
    if (!m_src || m_mode == ViewMode::Flat)
        return;

    QHash<QString, int> keyToIdx;
    const int rows = m_src->rowCount();
    for (int r = 0; r < rows; ++r) {
        const QString key = groupKeyFor(r);
        int idx = keyToIdx.value(key, -1);
        if (idx < 0) {
            idx = static_cast<int>(m_groups.size());
            keyToIdx.insert(key, idx);
            Group g;
            g.key = key;
            g.label = groupLabelFor(r, key);
            m_groups.append(std::move(g));
        }
        m_groups[idx].srcRows.append(r);
    }

    applyCurrentSort();
    refreshSrcIndex();
}

void ConnectionGroupProxy::applyCurrentSort()
{
    if (m_sortColumn < 0 || m_mode == ViewMode::Flat || m_groups.isEmpty())
        return;

    const auto cmpVariant = [order = m_sortOrder](const QVariant &a, const QVariant &b) {
        // QVariant::compare returns QPartialOrdering. Use it so numeric and
        // string sort roles compare correctly without ambiguous toString.
        const auto ord = QVariant::compare(a, b);
        const bool less = (ord == QPartialOrdering::Less);
        const bool equal = (ord == QPartialOrdering::Equivalent);
        if (equal) return false;
        return order == Qt::AscendingOrder ? less : !less;
    };

    // Sort each group's children by the source SortRole for the sort column.
    for (Group &g : m_groups) {
        std::sort(g.srcRows.begin(), g.srcRows.end(),
                  [&](int lhs, int rhs) {
                      const QVariant a = m_src->index(lhs, m_sortColumn)
                                             .data(ConnectionModel::SortRole);
                      const QVariant b = m_src->index(rhs, m_sortColumn)
                                             .data(ConnectionModel::SortRole);
                      return cmpVariant(a, b);
                  });
    }

    // Sort groups by their aggregated SortRole value.
    std::sort(m_groups.begin(), m_groups.end(),
              [&](const Group &lhs, const Group &rhs) {
                  const QVariant a = aggregateData(lhs, m_sortColumn,
                                                   ConnectionModel::SortRole);
                  const QVariant b = aggregateData(rhs, m_sortColumn,
                                                   ConnectionModel::SortRole);
                  return cmpVariant(a, b);
              });
}

void ConnectionGroupProxy::sort(int column, Qt::SortOrder order)
{
    // Remember even when m_src isn't ready yet — applyCurrentSort()
    // will pick it up the first time rebuild() runs.
    m_sortColumn = column;
    m_sortOrder  = order;

    if (!m_src) return;

    if (m_mode == ViewMode::Flat) {
        // Forward to the source (QSortFilterProxyModel handles the real
        // work + emits layoutChanged for its own persistents). We must
        // additionally remap OUR persistent indexes — the view holds
        // proxies into us, not into source.
        emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);

        // Snapshot identities via source's QPersistentModelIndex; these
        // follow the source's row remap automatically.
        const auto oldList = persistentIndexList();
        QList<QPersistentModelIndex> srcPersistents;
        srcPersistents.reserve(oldList.size());
        for (const QModelIndex &p : oldList)
            srcPersistents.append(QPersistentModelIndex(m_src->index(p.row(), p.column())));

        m_src->sort(column, order);

        QModelIndexList newList;
        newList.reserve(oldList.size());
        for (int i = 0; i < oldList.size(); ++i) {
            const QPersistentModelIndex &sp = srcPersistents[i];
            newList.append(sp.isValid()
                               ? createIndex(sp.row(), sp.column(), kTopLevelId)
                               : QModelIndex{});
        }
        changePersistentIndexList(oldList, newList);

        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        return;
    }

    // Grouped: rearrange m_groups + each group's srcRows, remap
    // persistent indexes by (groupKey, sourceRow) identity so selection
    // and tree expansion state survive sort.
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);

    const auto oldList = persistentIndexList();
    // identity[i] = pair(groupKey, sourceRow); sourceRow == -1 means
    // the persistent points at a group row itself.
    struct Identity { QString key; int srcRow; int col; };
    QList<Identity> identities;
    identities.reserve(oldList.size());
    for (const QModelIndex &p : oldList) {
        Identity id{{}, -1, p.column()};
        if (p.internalId() == kTopLevelId) {
            const int gi = p.row();
            if (gi >= 0 && gi < m_groups.size()) id.key = m_groups[gi].key;
        } else {
            const auto gi = static_cast<int>(p.internalId());
            if (gi >= 0 && gi < m_groups.size()
                && p.row() >= 0 && p.row() < m_groups[gi].srcRows.size()) {
                id.key = m_groups[gi].key;
                id.srcRow = m_groups[gi].srcRows[p.row()];
            }
        }
        identities.append(std::move(id));
    }

    applyCurrentSort();
    // Rebuild the O(1) reverse index for the post-sort layout; the
    // persistent-index remap below then uses it instead of an O(N)
    // indexOf per persistent.
    refreshSrcIndex();

    // Re-build the key→index lookup once for the post-sort group order.
    QHash<QString, int> keyToIdx;
    keyToIdx.reserve(m_groups.size());
    for (int gi = 0; gi < m_groups.size(); ++gi)
        keyToIdx.insert(m_groups[gi].key, gi);

    QModelIndexList newList;
    newList.reserve(oldList.size());
    for (const Identity &id : identities) {
        if (id.key.isEmpty()) { newList.append(QModelIndex{}); continue; }
        const int gi = keyToIdx.value(id.key, -1);
        if (gi < 0) { newList.append(QModelIndex{}); continue; }
        if (id.srcRow < 0) {
            newList.append(createIndex(gi, id.col, kTopLevelId));
        } else {
            const auto it = m_srcIndex.constFind(id.srcRow);
            newList.append(it != m_srcIndex.constEnd()
                               ? createIndex(it->second, id.col,
                                             static_cast<quintptr>(it->first))
                               : QModelIndex{});
        }
    }
    changePersistentIndexList(oldList, newList);

    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

QString ConnectionGroupProxy::groupKeyFor(int srcRow) const
{
    const Connection c = connectionAt(m_src, srcRow);
    switch (m_mode) {
    case ViewMode::Flat:
        return {};
    case ViewMode::ByInterface:
        return c.iface.isEmpty() ? QStringLiteral("(unattributed)") : c.iface;
    case ViewMode::ByContainer:
        // Prefer a stable id for the key; name is for display only.
        if (!c.container.id.isEmpty())
            return QStringLiteral("%1/%2").arg(c.container.runtime, c.container.id);
        if (!c.container.name.isEmpty())
            return QStringLiteral("%1/%2").arg(c.container.runtime, c.container.name);
        return QStringLiteral("(host)");
    case ViewMode::ByProcess:
        if (c.process.pid > 0)
            return QStringLiteral("%1/%2")
                .arg(c.process.pid).arg(c.process.comm);
        return QStringLiteral("(unattributed)");
    }
    return {};
}

QString ConnectionGroupProxy::groupLabelFor(int srcRow, const QString &key) const
{
    const Connection c = connectionAt(m_src, srcRow);
    switch (m_mode) {
    case ViewMode::Flat:
        return {};
    case ViewMode::ByInterface:
        return c.iface.isEmpty() ? QStringLiteral("(unattributed)") : c.iface;
    case ViewMode::ByContainer: {
        if (c.container.id.isEmpty() && c.container.name.isEmpty())
            return QStringLiteral("(host)");
        const QString display = !c.container.name.isEmpty() ? c.container.name
                                                            : c.container.id.left(12);
        return c.container.runtime.isEmpty()
                   ? display
                   : QStringLiteral("%1: %2").arg(c.container.runtime, display);
    }
    case ViewMode::ByProcess:
        if (c.process.pid > 0) {
            const QString name = c.process.comm.isEmpty()
                                     ? QStringLiteral("pid %1").arg(c.process.pid)
                                     : c.process.comm;
            return QStringLiteral("%1  [pid %2]").arg(name).arg(c.process.pid);
        }
        return QStringLiteral("(unattributed)");
    }
    return key;
}

bool ConnectionGroupProxy::isGroupIndex(const QModelIndex &idx) const
{
    if (!idx.isValid() || m_mode == ViewMode::Flat) return false;
    return idx.internalId() == kTopLevelId;
}

QModelIndex ConnectionGroupProxy::mapToSource(const QModelIndex &proxy) const
{
    if (!proxy.isValid() || !m_src) return {};
    if (m_mode == ViewMode::Flat) {
        // Top-level rows ARE source rows in Flat mode.
        return m_src->index(proxy.row(), proxy.column());
    }
    if (proxy.internalId() == kTopLevelId)
        return {};  // group rows have no source
    const auto groupIdx = static_cast<int>(proxy.internalId());
    if (groupIdx < 0 || groupIdx >= m_groups.size()) return {};
    const Group &g = m_groups[groupIdx];
    if (proxy.row() < 0 || proxy.row() >= g.srcRows.size()) return {};
    return m_src->index(g.srcRows[proxy.row()], proxy.column());
}

QModelIndex ConnectionGroupProxy::mapFromSource(const QModelIndex &src) const
{
    if (!src.isValid() || !m_src) return {};
    if (m_mode == ViewMode::Flat)
        return index(src.row(), src.column());
    // O(1) reverse lookup (was an O(N) scan across every group).
    const auto it = m_srcIndex.constFind(src.row());
    if (it == m_srcIndex.constEnd()) return {};
    const int gi       = it->first;
    const int childRow = it->second;
    return createIndex(childRow, src.column(), static_cast<quintptr>(gi));
}

QModelIndex ConnectionGroupProxy::index(int row, int column,
                                        const QModelIndex &parent) const
{
    if (row < 0 || column < 0) return {};
    if (m_mode == ViewMode::Flat) {
        if (parent.isValid()) return {};   // strict 1-level
        if (!m_src) return {};
        if (row >= m_src->rowCount()) return {};
        return createIndex(row, column, kTopLevelId);
    }
    if (!parent.isValid()) {
        if (row >= m_groups.size()) return {};
        return createIndex(row, column, kTopLevelId);
    }
    // Child of a group: parent.row() = group index, parent.internalId() == kTopLevelId.
    if (parent.internalId() != kTopLevelId) return {};
    const int gi = parent.row();
    if (gi < 0 || gi >= m_groups.size()) return {};
    if (row >= m_groups[gi].srcRows.size()) return {};
    return createIndex(row, column, static_cast<quintptr>(gi));
}

QModelIndex ConnectionGroupProxy::parent(const QModelIndex &child) const
{
    if (!child.isValid() || m_mode == ViewMode::Flat) return {};
    if (child.internalId() == kTopLevelId) return {};
    return createIndex(static_cast<int>(child.internalId()), 0, kTopLevelId);
}

int ConnectionGroupProxy::rowCount(const QModelIndex &parent) const
{
    if (m_mode == ViewMode::Flat)
        return parent.isValid() ? 0 : (m_src ? m_src->rowCount() : 0);
    if (!parent.isValid())
        return static_cast<int>(m_groups.size());
    if (parent.internalId() != kTopLevelId) return 0;  // children have no kids
    const int gi = parent.row();
    if (gi < 0 || gi >= m_groups.size()) return 0;
    return static_cast<int>(m_groups[gi].srcRows.size());
}

int ConnectionGroupProxy::columnCount(const QModelIndex &) const
{
    return m_src ? m_src->columnCount() : 0;
}

void ConnectionGroupProxy::setShowGroupDetails(bool on)
{
    if (m_showGroupDetails == on) return;
    m_showGroupDetails = on;
    if (m_mode == ViewMode::Flat || m_groups.isEmpty())
        return;
    // Repaint every group header row (column 0 .. last) so the inline
    // detail + tooltip appear/disappear immediately.
    const int lastCol = columnCount() - 1;
    emit dataChanged(index(0, 0), index(m_groups.size() - 1, lastCol));
}

qint32 ConnectionGroupProxy::representativePid(const QModelIndex &groupIdx) const
{
    if (m_mode != ViewMode::ByProcess || !m_src) return 0;
    if (!groupIdx.isValid() || groupIdx.internalId() != kTopLevelId) return 0;
    const int gi = groupIdx.row();
    if (gi < 0 || gi >= m_groups.size() || m_groups[gi].srcRows.isEmpty())
        return 0;
    const Connection c = m_src->index(m_groups[gi].srcRows.first(), 0)
                             .data(ConnectionModel::ConnectionRole)
                             .value<Connection>();
    return c.process.pid;
}

void ConnectionGroupProxy::refreshGroupTooltips()
{
    if (m_mode == ViewMode::Flat || m_groups.isEmpty()) return;
    const int lastCol = columnCount() - 1;
    emit dataChanged(index(0, 0), index(m_groups.size() - 1, lastCol),
                     {Qt::ToolTipRole});
}

QVariantList ConnectionGroupProxy::groupChips(const Group &g) const
{
    QVariantList chips;
    if (g.srcRows.isEmpty() || !m_src) return chips;

    const auto chip = [](const QString &text, const char *kind) {
        QVariantMap m;
        m.insert(QStringLiteral("text"), text);
        m.insert(QStringLiteral("kind"), QString::fromLatin1(kind));
        return QVariant(m);
    };

    const Connection c = m_src->index(g.srcRows.first(), 0)
                             .data(ConnectionModel::ConnectionRole)
                             .value<Connection>();

    switch (m_mode) {
    case ViewMode::ByProcess:
        if (!c.process.comm.isEmpty())
            chips << chip(c.process.comm, "process");
        if (c.process.pid > 0 && m_showGroupDetails) {
            chips << chip(QStringLiteral("pid %1").arg(c.process.pid), "pid");
            // Only resolve uid → name for HOST processes. A container's
            // uid maps to ITS /etc/passwd, not the host's — resolving it
            // against the host db would show a nonsensical (or wrong)
            // name. Show the bare numeric uid for container flows.
            const bool inContainer = !c.container.runtime.isEmpty()
                                     || !c.container.id.isEmpty();
            const QString user = inContainer
                ? QString()
                : qiftop::platform::userNameForUid(c.process.uid);
            chips << chip(user.isEmpty()
                              ? QStringLiteral("uid %1").arg(c.process.uid)
                              : QStringLiteral("%1 (uid %2)").arg(user).arg(c.process.uid),
                          "user");
            // On-demand cmdline (elided) once the RPC has answered.
            if (m_details) {
                if (const auto it = m_details->constFind(c.process.pid);
                    it != m_details->constEnd() && it->valid()
                    && !it->cmdline.isEmpty()) {
                    QString cmd = it->cmdline;
                    if (cmd.size() > 60) cmd = cmd.left(57) + QStringLiteral("…");
                    chips << chip(cmd, "cmdline");
                }
            }
        }
        break;
    case ViewMode::ByContainer: {
        const QString primary = !c.container.name.isEmpty()
            ? (c.container.runtime.isEmpty()
                   ? c.container.name
                   : QStringLiteral("%1: %2").arg(c.container.runtime, c.container.name))
            : g.label;
        chips << chip(primary, "container");
        if (m_showGroupDetails && !c.container.id.isEmpty())
            chips << chip(QStringLiteral("id %1").arg(c.container.id.left(12)), "id");
        break;
    }
    case ViewMode::ByInterface:
        chips << chip(g.label, "iface");
        break;
    default:
        break;
    }

    chips << chip(QStringLiteral("%1 flow%2").arg(g.srcRows.size())
                      .arg(g.srcRows.size() == 1 ? QString() : QStringLiteral("s")),
                  "count");
    return chips;
}

QString ConnectionGroupProxy::groupDetailInline(const Group &g) const
{
    // Plain-text fallback (copy / export / accessibility): the chip
    // texts joined. The delegate renders the colour-coded version from
    // GroupChipsRole.
    QStringList parts;
    for (const QVariant &v : groupChips(g))
        parts << v.toMap().value(QStringLiteral("text")).toString();
    return parts.join(QStringLiteral("  ·  "));
}

QString ConnectionGroupProxy::groupDetailTooltip(const Group &g) const
{
    if (!m_showGroupDetails || g.srcRows.isEmpty() || !m_src)
        return {};
    const Connection c = m_src->index(g.srcRows.first(), 0)
                             .data(ConnectionModel::ConnectionRole)
                             .value<Connection>();
    QStringList lines;
    switch (m_mode) {
    case ViewMode::ByProcess:
        if (c.process.pid > 0) {
            if (!c.process.comm.isEmpty())
                lines << QStringLiteral("Process: %1").arg(c.process.comm);
            lines << QStringLiteral("PID: %1").arg(c.process.pid);
            // Host-only uid→name resolution (see groupChips): a
            // container's uid is meaningless against the host passwd db.
            const bool inContainer = !c.container.runtime.isEmpty()
                                     || !c.container.id.isEmpty();
            const QString user = inContainer
                ? QString()
                : qiftop::platform::userNameForUid(c.process.uid);
            lines << (user.isEmpty()
                          ? QStringLiteral("User: uid %1").arg(c.process.uid)
                          : QStringLiteral("User: %1 (uid %2)")
                                .arg(user).arg(c.process.uid));
            // On-demand extended fields, if the agent has answered for
            // this pid (exe / cmdline / cwd). Absent until the RPC
            // returns — the tooltip degrades to the wire fields above.
            if (m_details) {
                if (const auto it = m_details->constFind(c.process.pid);
                    it != m_details->constEnd() && it->valid()) {
                    if (!it->exe.isEmpty())
                        lines << QStringLiteral("Exe: %1").arg(it->exe);
                    if (!it->cmdline.isEmpty())
                        lines << QStringLiteral("Cmdline: %1").arg(it->cmdline);
                    if (!it->cwd.isEmpty())
                        lines << QStringLiteral("Cwd: %1").arg(it->cwd);
                }
            }
        }
        break;
    case ViewMode::ByContainer:
        if (!c.container.id.isEmpty() || !c.container.name.isEmpty()) {
            if (!c.container.runtime.isEmpty())
                lines << QStringLiteral("Runtime: %1").arg(c.container.runtime);
            if (!c.container.name.isEmpty())
                lines << QStringLiteral("Name: %1").arg(c.container.name);
            if (!c.container.id.isEmpty())
                lines << QStringLiteral("ID: %1").arg(c.container.id);
        }
        break;
    case ViewMode::ByInterface:
        lines << QStringLiteral("Interface: %1").arg(g.label);
        break;
    default:
        break;
    }
    if (lines.isEmpty()) return {};
    lines << QStringLiteral("%1 flow%2").arg(g.srcRows.size())
                 .arg(g.srcRows.size() == 1 ? QString() : QStringLiteral("s"));
    return lines.join(QLatin1Char('\n'));
}

QVariant ConnectionGroupProxy::aggregateData(const Group &g, int column, int role) const
{
    using Col = ConnectionModel::Column;
    double rxSum = 0, txSum = 0;
    quint64 rxBytes = 0, txBytes = 0;
    for (int r : g.srcRows) {
        const QModelIndex i0 = m_src->index(r, 0);
        rxSum   += i0.data(ConnectionModel::RxRateRole).toDouble();
        txSum   += i0.data(ConnectionModel::TxRateRole).toDouble();
        const Connection c = i0.data(ConnectionModel::ConnectionRole).value<Connection>();
        rxBytes += c.rxBytes;
        txBytes += c.txBytes;
    }

    const auto col = static_cast<Col>(column);
    if (role == Qt::DisplayRole) {
        switch (col) {
        case Col::Iface:
            return QStringLiteral("%1  [%2]")
                .arg(g.label).arg(g.srcRows.size());
        case Col::Flow:
            // Plain-text fallback (copy/export/accessibility). The
            // delegate paints the colour-coded chip version from
            // GroupChipsRole.
            return groupDetailInline(g);
        case Col::RxRate:  return util::formatByteRate(rxSum);
        case Col::TxRate:  return util::formatByteRate(txSum);
        case Col::RxTotal: return util::formatBytes(rxBytes);
        case Col::TxTotal: return util::formatBytes(txBytes);
        case Col::RxMax:   return QStringLiteral("—");
        case Col::TxMax:   return QStringLiteral("—");
        case Col::Process: return QStringLiteral("—");
        case Col::Container: return QStringLiteral("—");
        case Col::ColumnCount: break;
        }
        return {};
    }
    if (role == ConnectionModel::SortRole) {
        switch (col) {
        case Col::Iface:   return g.label;
        case Col::Flow:    return g.srcRows.size();
        case Col::RxRate:  return rxSum;
        case Col::TxRate:  return txSum;
        case Col::RxTotal: return static_cast<qulonglong>(rxBytes);
        case Col::TxTotal: return static_cast<qulonglong>(txBytes);
        case Col::RxMax:   return 0.0;
        case Col::TxMax:   return 0.0;
        case Col::Process: return QString();
        case Col::Container: return QString();
        case Col::ColumnCount: break;
        }
    }
    if (role == Qt::ToolTipRole) {
        return groupDetailTooltip(g);
    }
    if (role == ConnectionModel::GroupChipsRole) {
        return groupChips(g);
    }
    if (role == ConnectionModel::RxRateRole) return rxSum;
    if (role == ConnectionModel::TxRateRole) return txSum;
    if (role == Qt::FontRole) {
        QFont f;
        f.setBold(true);
        return f;
    }
    return {};
}

int ConnectionGroupProxy::groupIndexForSourceRow(int srcRow) const
{
    // O(1) reverse lookup (was an O(N) scan calling contains() on
    // every group's srcRows).
    const auto it = m_srcIndex.constFind(srcRow);
    return it == m_srcIndex.constEnd() ? -1 : it->first;
}

void ConnectionGroupProxy::refreshSrcIndex()
{
    m_srcIndex.clear();
    if (m_mode == ViewMode::Flat) return;
    int total = 0;
    for (const Group &g : m_groups) total += g.srcRows.size();
    m_srcIndex.reserve(total);
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        const QList<int> &rows = m_groups[gi].srcRows;
        for (int childRow = 0; childRow < rows.size(); ++childRow)
            m_srcIndex.insert(rows[childRow], qMakePair(gi, childRow));
    }
}

void ConnectionGroupProxy::forwardSourceDataChanged(const QModelIndex &topLeft,
                                                    const QModelIndex &bottomRight,
                                                    const QVector<int> &roles)
{
    QSet<int> parentGroups;
    for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
        const QModelIndex left = mapFromSource(m_src->index(row, topLeft.column()));
        const QModelIndex right = mapFromSource(m_src->index(row, bottomRight.column()));
        if (left.isValid() && right.isValid())
            emit dataChanged(left, right, roles);

        const int gi = groupIndexForSourceRow(row);
        if (gi >= 0)
            parentGroups.insert(gi);
    }

    for (int gi : std::as_const(parentGroups)) {
        emit dataChanged(index(gi, topLeft.column()),
                         index(gi, bottomRight.column()),
                         roles);
    }
}

QVariant ConnectionGroupProxy::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || !m_src) return {};
    if (m_mode == ViewMode::Flat || index.internalId() != kTopLevelId) {
        // Flow row — forward to source verbatim, preserving every
        // custom role used by the delegates (gauge, direction, etc.).
        const QModelIndex src = mapToSource(index);
        return src.isValid() ? src.data(role) : QVariant{};
    }
    // Group row
    const int gi = index.row();
    if (gi < 0 || gi >= m_groups.size()) return {};
    return aggregateData(m_groups[gi], index.column(), role);
}

QVariant ConnectionGroupProxy::headerData(int section, Qt::Orientation orientation,
                                          int role) const
{
    return m_src ? m_src->headerData(section, orientation, role) : QVariant{};
}

Qt::ItemFlags ConnectionGroupProxy::flags(const QModelIndex &index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}
