#include "ConnectionFilterProxy.h"
#include "ConnectionModel.h"
#include "backend/Connection.h"

// NOTE: QSortFilterProxyModel::beginFilterChange / endFilterChange were
// added in Qt 6.5. We target Qt >= 6.2 (Ubuntu 22.04 / 24.04 baseline),
// so we use invalidateFilter() which has been available since Qt 5 and
// has the same observable effect (just no batched-edit optimisation).

void ConnectionFilterProxy::setShowIPv6(bool show)
{
    if (m_showIPv6 == show)
        return;
    m_showIPv6 = show;
    invalidateFilter();
}

void ConnectionFilterProxy::setShowTcp(bool show)
{
    if (m_showTcp == show) return;
    m_showTcp = show;
    invalidateFilter();
}

void ConnectionFilterProxy::setShowUdp(bool show)
{
    if (m_showUdp == show) return;
    m_showUdp = show;
    invalidateFilter();
}

void ConnectionFilterProxy::setVisibleIfaces(const QSet<QString> &ifaces)
{
    if (m_visibleIfaces == ifaces)
        return;
    m_visibleIfaces = ifaces;
    invalidateFilter();
}

QString ConnectionFilterProxy::setFilterExpression(const QString &expr)
{
    if (expr == m_exprText)
        return m_exprError;
    m_exprText  = expr;
    auto res    = qiftop::filter::parse(expr.trimmed());
    m_expr      = res.expr;
    m_exprError = res.error;
    invalidateFilter();
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
