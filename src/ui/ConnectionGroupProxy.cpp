#include "ConnectionGroupProxy.h"

#include "ui/ConnectionModel.h"
#include "backend/Connection.h"
#include "util/Units.h"

#include <QFont>
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
        connect(m_src, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex &, int, int){ onSourceRowsInsertedOrRemoved(); });
        connect(m_src, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex &, int, int){ onSourceRowsInsertedOrRemoved(); });
        connect(m_src, &QAbstractItemModel::layoutChanged, this,
                [this]{ onSourceReset(); });
        connect(m_src, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex &, const QModelIndex &, const QVector<int> &){
                    onSourceDataChanged();
                });
    }
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::setViewMode(ViewMode mode)
{
    if (mode == m_mode) return;
    beginResetModel();
    m_mode = mode;
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::onSourceReset()
{
    beginResetModel();
    rebuild();
    endResetModel();
}

void ConnectionGroupProxy::onSourceRowsInsertedOrRemoved()
{
    // Treat structural changes as a wholesale rebuild — the alternative
    // (incremental insert/remove with row mapping) buys nothing for a
    // model that's already capped at ~4 k rows by the agent and is
    // refreshed on every backend tick.
    onSourceReset();
}

void ConnectionGroupProxy::onSourceDataChanged()
{
    // In Flat mode there's no aggregation cache; just forward as a
    // blanket repaint. In grouped mode, the group key for a row could
    // have changed (e.g. a flow just gained a PID via late attribution)
    // so rebuild to be safe — same justification as the
    // rowsInserted/Removed handler.
    if (m_mode == ViewMode::Flat) {
        if (!m_src) return;
        const int rows = m_src->rowCount();
        const int cols = m_src->columnCount();
        if (rows > 0 && cols > 0) {
            emit dataChanged(index(0, 0), index(rows - 1, cols - 1));
        }
        return;
    }
    onSourceReset();
}

void ConnectionGroupProxy::rebuild()
{
    m_groups.clear();
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
    // Locate the group containing this source row.
    for (int gi = 0; gi < m_groups.size(); ++gi) {
        const int childRow = static_cast<int>(m_groups[gi].srcRows.indexOf(src.row()));
        if (childRow >= 0)
            return createIndex(childRow, src.column(), static_cast<quintptr>(gi));
    }
    return {};
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
            return QStringLiteral("%1 flow%2 in “%3”")
                .arg(g.srcRows.size())
                .arg(g.srcRows.size() == 1 ? QStringLiteral("") : QStringLiteral("s"),
                     g.label);
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
    if (role == ConnectionModel::RxRateRole) return rxSum;
    if (role == ConnectionModel::TxRateRole) return txSum;
    if (role == Qt::FontRole) {
        QFont f;
        f.setBold(true);
        return f;
    }
    return {};
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
