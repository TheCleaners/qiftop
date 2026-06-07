#pragma once

#include <QStyledItemDelegate>
#include <QPointer>

class QAbstractItemView;

// Paints a row-spanning "gauge" background underneath the cell's content:
// the row is filled (with a darker overlay color sourced from the model's
// GaugeDarkColorRole) up to a horizontal boundary determined by the row's
// GaugeFractionRole. The boundary is computed in row-relative coordinates
// using the header section geometry, so the visual effect is a single
// continuous gauge spanning the entire row even though each cell is
// painted independently.
//
// The "unfilled" base background is whatever Qt::BackgroundRole returns
// (we paint it ourselves and then suppress the default style's redundant
// fill via opt.backgroundBrush). When the row is selected, the highlight
// takes over and we don't paint a gauge.
//
// Use as the default item delegate on a QTableView; columns that need
// custom content rendering can subclass this and call
// paintGaugeBackground() before their own paint code.
class RowGaugeDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit RowGaugeDelegate(QAbstractItemView *view, QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

protected:
    // Paints the gauge backdrop (base tint + dark overlay). Returns true
    // if anything was painted. Subclasses should call this and then suppress
    // the default style's redundant background fill before rendering their
    // own foreground content.
    bool paintGaugeBackground(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const;

private:
    QPointer<QAbstractItemView> m_view;
};
