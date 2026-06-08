#pragma once

#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

#include "util/ConnectionFilter.h"

// Row-level filter for the Connections table.
//
// Filters stacked (cheapest first, so a broad filter rejects rows before
// the expensive expression eval runs):
//   • Show-IPv6 / showTcp / showUdp toggles.
//   • Per-interface visibility set: empty = "show all".
//   • Free-form filter expression (see util/ConnectionFilter.h grammar).
class ConnectionFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setShowIPv6(bool show);
    void setShowTcp(bool show);
    void setShowUdp(bool show);

    // Empty set == show all. Otherwise only rows whose iface name is in the
    // set are visible. Pass the empty-string sentinel to keep "unattributed"
    // flows visible while filtering interfaces.
    void setVisibleIfaces(const QSet<QString> &ifaces);

    // Free-form expression filter. Empty / null-parse → no filter.
    // Returns the parse error string for the UI to surface in a tooltip
    // (empty on success / empty input).
    QString setFilterExpression(const QString &expr);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool          m_showIPv6 = true;
    bool          m_showTcp  = true;
    bool          m_showUdp  = true;
    QSet<QString> m_visibleIfaces;   // empty = all
    QString                   m_exprText;
    QString                   m_exprError;
    qiftop::filter::ExprPtr   m_expr;
};
