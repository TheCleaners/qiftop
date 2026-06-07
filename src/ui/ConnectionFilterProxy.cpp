#include "ConnectionFilterProxy.h"
#include "ConnectionModel.h"
#include "backend/Connection.h"

void ConnectionFilterProxy::setShowIPv6(bool show)
{
    if (m_showIPv6 == show)
        return;
    beginFilterChange();
    m_showIPv6 = show;
    endFilterChange();
}

void ConnectionFilterProxy::setShowTcp(bool show)
{
    if (m_showTcp == show) return;
    beginFilterChange();
    m_showTcp = show;
    endFilterChange();
}

void ConnectionFilterProxy::setShowUdp(bool show)
{
    if (m_showUdp == show) return;
    beginFilterChange();
    m_showUdp = show;
    endFilterChange();
}

void ConnectionFilterProxy::setVisibleIfaces(const QSet<QString> &ifaces)
{
    if (m_visibleIfaces == ifaces)
        return;
    beginFilterChange();
    m_visibleIfaces = ifaces;
    endFilterChange();
}

#include "ConnectionFilterProxy.h"
#include "ConnectionModel.h"
#include "backend/Connection.h"

QString ConnectionFilterProxy::setFilterExpression(const QString &expr)
{
    if (expr == m_exprText)
        return m_exprError;
    beginFilterChange();
    m_exprText = expr;
    auto res   = qiftop::filter::parse(expr.trimmed());
    m_expr     = res.expr;
    m_exprError = res.error;
    endFilterChange();
    return m_exprError;
}

bool ConnectionFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const auto idx = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!m_showIPv6 && idx.data(ConnectionModel::IsIPv6Role).toBool())
        return false;
    const auto proto = static_cast<L4Proto>(idx.data(ConnectionModel::ProtoRole).toInt());
    if (!m_showTcp && proto == L4Proto::Tcp) return false;
    if (!m_showUdp && proto == L4Proto::Udp) return false;
    if (!m_visibleIfaces.isEmpty()) {
        const QString iface = idx.data(ConnectionModel::IfaceNameRole).toString();
        if (!m_visibleIfaces.contains(iface))
            return false;
    }
    if (m_expr) {
        const Connection c =
            idx.data(ConnectionModel::ConnectionRole).value<Connection>();
        qiftop::filter::Context ctx{
            c,
            idx.data(ConnectionModel::RxRateRole).toDouble(),
            idx.data(ConnectionModel::TxRateRole).toDouble(),
            idx.data(ConnectionModel::HostnameLocalRole).toString(),
            idx.data(ConnectionModel::HostnameRemoteRole).toString(),
        };
        if (!qiftop::filter::matches(m_expr, ctx))
            return false;
    }
    return true;
}
