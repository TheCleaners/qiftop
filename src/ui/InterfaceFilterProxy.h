#pragma once

#include <QSortFilterProxyModel>

class InterfaceFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setShowLoopback(bool show);
    void setShowDown(bool show);

    [[nodiscard]] bool showLoopback() const { return m_showLoopback; }
    [[nodiscard]] bool showDown() const     { return m_showDown; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool m_showLoopback = false;
    bool m_showDown     = true;
};
