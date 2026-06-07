#include "InterfaceFilterProxy.h"
#include "NetworkModel.h"

void InterfaceFilterProxy::setShowLoopback(bool show)
{
    if (m_showLoopback == show)
        return;
    beginFilterChange();
    m_showLoopback = show;
    endFilterChange();
}

void InterfaceFilterProxy::setShowDown(bool show)
{
    if (m_showDown == show)
        return;
    beginFilterChange();
    m_showDown = show;
    endFilterChange();
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
