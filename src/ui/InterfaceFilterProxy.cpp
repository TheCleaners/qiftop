#include "InterfaceFilterProxy.h"
#include "NetworkModel.h"

// See note in ConnectionFilterProxy.cpp: beginFilterChange/endFilterChange
// are Qt 6.5+. We target Qt >= 6.2 so use invalidateFilter() instead.

void InterfaceFilterProxy::setShowLoopback(bool show)
{
    if (m_showLoopback == show)
        return;
    m_showLoopback = show;
    invalidateFilter();
}

void InterfaceFilterProxy::setShowDown(bool show)
{
    if (m_showDown == show)
        return;
    m_showDown = show;
    invalidateFilter();
}

bool InterfaceFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!m_showLoopback && idx.data(NetworkModel::IsLoopbackRole).toBool())
        return false;
    if (!m_showDown && !idx.data(NetworkModel::IsUpRole).toBool())
        return false;
    return true;
}
