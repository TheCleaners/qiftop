#include "ConnectionAttributionDelegate.h"

#include "ConnectionModel.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QStringList>

ConnectionAttributionDelegate::ConnectionAttributionDelegate(QAbstractItemView *view,
                                                             QObject *parent)
    : RowGaugeDelegate(view, parent)
{}

namespace {

// 85% of the option font, but never smaller than 7pt — readability
// floor for HiDPI/4K users.
QFont annotationFont(const QFont &base)
{
    QFont f = base;
    if (f.pointSizeF() > 0)
        f.setPointSizeF(std::max(7.0, f.pointSizeF() * 0.85));
    else
        f.setPixelSize(std::max(9, int(f.pixelSize() * 0.85)));
    return f;
}

// Returns true if the index is the Process column and the row has a
// non-zero pid, OR the Container column with a non-empty container.
bool hasAttribution(const QModelIndex &idx)
{
    const auto col = static_cast<ConnectionModel::Column>(idx.column());
    if (col == ConnectionModel::Column::Process)
        return idx.data(ConnectionModel::ProcessPidRole).toInt() > 0;
    if (col == ConnectionModel::Column::Container) {
        return !idx.data(ConnectionModel::ContainerRuntimeRole).toString().isEmpty()
            || !idx.data(ConnectionModel::ContainerIdRole).toString().isEmpty()
            || !idx.data(ConnectionModel::ContainerNameRole).toString().isEmpty();
    }
    return false;
}

} // namespace

void ConnectionAttributionDelegate::paint(QPainter *painter,
                                          const QStyleOptionViewItem &option,
                                          const QModelIndex &index) const
{
    // Paint the row-spanning gauge backdrop (no-op when the gauge is
    // disabled or the row isn't ready).
    paintGaugeBackground(painter, option, index);

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // Suppress the default style's redundant background — the gauge
    // backdrop already painted it.
    opt.backgroundBrush = Qt::NoBrush;

    const auto col = static_cast<ConnectionModel::Column>(index.column());
    if (col != ConnectionModel::Column::Process
        && col != ConnectionModel::Column::Container) {
        // Fall back to the default render path for any non-attribution
        // column that might end up routed here by accident.
        RowGaugeDelegate::paint(painter, option, index);
        return;
    }

    // Group rows in ConnectionGroupProxy's tree modes (ByInterface /
    // ByContainer / ByProcess) have children — the per-flow attribution
    // structured roles are empty (the group aggregates over heterogeneous
    // flows that may span multiple containers/processes). Render the
    // group's own DisplayRole text ("—") via the default path instead
    // of synthesising "(host)" from the empty structured roles, which
    // would mislead the user into thinking the group is host-native.
    if (index.model() && index.model()->hasChildren(index)) {
        RowGaugeDelegate::paint(painter, option, index);
        return;
    }

    const QStyle *style = opt.widget ? opt.widget->style()
                                     : QApplication::style();

    // Draw selection/focus chrome (without text), then we paint the
    // rich content ourselves.
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    QRect r = opt.rect.adjusted(4, 0, -4, 0);
    if (!r.isValid() || r.width() <= 0) return;

    painter->save();

    const bool selected = (opt.state & QStyle::State_Selected);
    const QPalette &pal = opt.palette;
    const QColor primary = selected
        ? pal.color(QPalette::HighlightedText)
        : pal.color(QPalette::Text);
    const QColor annotation = selected
        ? pal.color(QPalette::HighlightedText)
        : pal.color(QPalette::PlaceholderText);

    const bool attributed = hasAttribution(index);

    QFont primaryFont = opt.font;
    QFont annoFont    = annotationFont(opt.font);

    // Pull the structured values from the model so we can render them
    // with distinct fonts/colors instead of parsing the DisplayRole
    // back apart.
    QString primaryText;
    QString annoText;
    bool italicWhenUnattributed = true;

    if (col == ConnectionModel::Column::Process) {
        const int pid     = index.data(ConnectionModel::ProcessPidRole).toInt();
        const QString comm = index.data(ConnectionModel::ProcessCommRole).toString();
        if (pid > 0) {
            primaryText = comm.isEmpty() ? QStringLiteral("pid %1").arg(pid) : comm;
            annoText    = QStringLiteral("[%1]").arg(pid);
        } else {
            primaryText = QStringLiteral("—");
        }
    } else {  // Container
        const QString runtime = index.data(ConnectionModel::ContainerRuntimeRole).toString();
        const QString id      = index.data(ConnectionModel::ContainerIdRole).toString();
        const QString name    = index.data(ConnectionModel::ContainerNameRole).toString();
        const QStringList chain = index.data(ConnectionModel::ContainerChainRole).toStringList();
        if (runtime.isEmpty() && id.isEmpty() && name.isEmpty()) {
            primaryText = QStringLiteral("(host)");
        } else {
            italicWhenUnattributed = false;
            const QString display = !name.isEmpty() ? name : id.left(12);
            primaryText = runtime.isEmpty()
                              ? display
                              : QStringLiteral("%1:%2").arg(runtime, display);
            if (chain.size() >= 2)
                annoText = QStringLiteral("▸ %1×").arg(chain.size());
        }
    }

    if (!attributed && italicWhenUnattributed)
        primaryFont.setItalic(true);

    QFontMetrics primaryFm(primaryFont);
    QFontMetrics annoFm(annoFont);
    const int annoWidth = annoText.isEmpty()
        ? 0
        : annoFm.horizontalAdvance(annoText) + 6;
    const int primaryMaxWidth = std::max(0, r.width() - annoWidth);
    const QString elided = primaryFm.elidedText(primaryText,
                                                Qt::ElideRight,
                                                primaryMaxWidth);

    painter->setFont(primaryFont);
    painter->setPen(attributed ? primary : annotation);
    painter->drawText(r.adjusted(0, 0, -annoWidth, 0),
                      Qt::AlignVCenter | Qt::AlignLeft, elided);

    if (!annoText.isEmpty()) {
        painter->setFont(annoFont);
        painter->setPen(annotation);
        QRect annoRect = r;
        annoRect.setLeft(r.right() - annoWidth + 4);
        painter->drawText(annoRect,
                          Qt::AlignVCenter | Qt::AlignLeft, annoText);
    }

    painter->restore();
}

QSize ConnectionAttributionDelegate::sizeHint(const QStyleOptionViewItem &option,
                                              const QModelIndex &index) const
{
    QSize base = RowGaugeDelegate::sizeHint(option, index);
    // Reserve a small minimum so the column is usable when empty.
    base.setWidth(std::max(base.width(), 120));
    return base;
}
