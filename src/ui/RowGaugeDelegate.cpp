#include "RowGaugeDelegate.h"
#include "GaugeRoles.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
#include <QPainter>
#include <QTableView>
#include <QTreeView>

RowGaugeDelegate::RowGaugeDelegate(QAbstractItemView *view, QObject *parent)
    : QStyledItemDelegate(parent)
    , m_view(view)
{
}

bool RowGaugeDelegate::paintGaugeBackground(QPainter *painter,
                                            const QStyleOptionViewItem &option,
                                            const QModelIndex &index) const
{
    // Selection wins over the gauge: let the default style paint the
    // highlight bar so it remains visually consistent with system theming.
    if (option.state & QStyle::State_Selected)
        return false;

    // Base tint (the "unfilled" portion of the row).
    const QVariant bgVar = index.data(Qt::BackgroundRole);
    bool painted = false;
    if (bgVar.isValid()) {
        painter->fillRect(option.rect, qvariant_cast<QBrush>(bgVar));
        painted = true;
    }

    const QVariant fracVar = index.data(qiftop::ui::GaugeFractionRole);
    if (!fracVar.isValid())
        return painted;
    const double frac = qBound(0.0, fracVar.toDouble(), 1.0);
    if (frac <= 0.0)
        return painted;

    const QVariant darkVar = index.data(qiftop::ui::GaugeDarkColorRole);
    if (!darkVar.isValid())
        return painted;
    const QColor dark = qvariant_cast<QColor>(darkVar);

    // Compute the gauge boundary in row-local coordinates by querying the
    // view's horizontal header geometry. This is what makes the gauge
    // appear as one continuous bar across all columns instead of restarting
    // per cell. Works for both QTableView (v0.1 connections view) and
    // QTreeView (v0.2 connections view, used to host grouped modes).
    QHeaderView *header = nullptr;
    if (auto *tv = qobject_cast<QTableView*>(m_view.data()))
        header = tv->horizontalHeader();
    else if (auto *tree = qobject_cast<QTreeView*>(m_view.data()))
        header = tree->header();
    if (!header) return painted;

    const int rowWidth = header->length();
    if (rowWidth <= 0) return painted;

    const int col          = index.column();
    const int cellOffsetX  = header->sectionViewportPosition(col);
    const int cellWidth    = header->sectionSize(col);
    const int boundaryRow  = qRound(frac * rowWidth);
    const int boundaryCell = boundaryRow - cellOffsetX;

    if (boundaryCell <= 0)
        return painted;

    QRect darkRect = option.rect;
    if (boundaryCell < cellWidth)
        darkRect.setWidth(boundaryCell);
    painter->fillRect(darkRect, dark);
    return true;
}

void RowGaugeDelegate::paint(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    const bool ownBg = paintGaugeBackground(painter, option, index);

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    if (ownBg) {
        // Suppress the default style's background fill — we already drew
        // our own (base tint + dark overlay). Clearing the brush prevents
        // CE_ItemViewItem from painting over our gauge.
        opt.backgroundBrush = QBrush();
    }
    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
}
