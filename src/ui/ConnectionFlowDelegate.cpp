#include "ConnectionFlowDelegate.h"
#include "ConnectionModel.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QTextDocument>

namespace {

// Pick endpoint accent colors that read on both light and dark themes.
// We detect the theme from the row's background luminance: dark base ->
// use brighter accents; light base -> use deeper accents.
struct FlowColors {
    QString muted;   // proto tag, arrow
    QString src;    // source endpoint
    QString dst;    // destination endpoint
};

FlowColors pickFlowColors(const QStyleOptionViewItem &option, bool selected)
{
    // On the selected row everything has to read against the highlight
    // background — bail out to a single contrasting color so we don't end
    // up with low-contrast colored text on the highlight bar.
    if (selected) {
        const QColor hl = option.palette.color(QPalette::HighlightedText);
        const QColor muted = option.palette.color(QPalette::Disabled, QPalette::HighlightedText);
        return { muted.name(QColor::HexArgb),
                 hl.name(QColor::HexArgb),
                 hl.name(QColor::HexArgb) };
    }
    const QColor base = option.palette.color(QPalette::Base);
    const bool   dark = base.lightness() < 128;
    const QColor muted = option.palette.color(QPalette::PlaceholderText);
    // Hand-picked, theme-aware accents. Source = cool, destination = warm.
    const QColor src = dark ? QColor(0x6CB6FF) : QColor(0x0B5FA5); // blue
    const QColor dst = dark ? QColor(0xF0B86E) : QColor(0xA0521B); // amber
    return { muted.name(QColor::HexArgb),
             src  .name(QColor::HexArgb),
             dst  .name(QColor::HexArgb) };
}

QTextDocument *buildDoc(const QStyleOptionViewItem &option,
                        const QModelIndex &index,
                        bool colorCode)
{
    const QString proto  = index.data(ConnectionModel::ProtoTextRole).toString().toHtmlEscaped();
    const QString local  = index.data(ConnectionModel::LocalTextRole).toString().toHtmlEscaped();
    const QString remote = index.data(ConnectionModel::RemoteTextRole).toString().toHtmlEscaped();

    const bool selected = option.state & QStyle::State_Selected;
    const FlowColors c  = pickFlowColors(option, selected);

    // Direction-aware ordering: outbound -> src=local; inbound -> src=remote;
    // unknown -> default to local→remote (matches non-aggregated view).
    const auto dirVar = index.data(ConnectionModel::DirectionRole);
    const int  dir    = dirVar.isValid() ? dirVar.toInt() : 0;
    const bool srcIsLocal = (dir != int(Direction::Inbound));
    const QString srcText = srcIsLocal ? local  : remote;
    const QString dstText = srcIsLocal ? remote : local;

    // Proto label: always upright, never italic, even when the row is stale
    // (the FontRole italic is applied to the whole row by the view; we
    // override it here for the proto tag so the user always sees the same
    // typographic signature).
    const QString protoSpan = QStringLiteral(
        "<span style=\"color:%1; font-style:normal;\">[%2]</span>")
        .arg(c.muted, proto);

    QString html;
    if (colorCode) {
        html = QStringLiteral(
            "%1"
            " <b style=\"color:%2;\">%3</b>"
            " <span style=\"color:%4;\">→</span>"
            " <b style=\"color:%5;\">%6</b>"
        ).arg(protoSpan, c.src, srcText, c.muted, c.dst, dstText);
    } else {
        html = QStringLiteral(
            "%1"
            " <b>%2</b>"
            " <span style=\"color:%3;\">→</span>"
            " <b>%4</b>"
        ).arg(protoSpan, srcText, c.muted, dstText);
    }

    auto *doc = new QTextDocument;
    doc->setDocumentMargin(0);
    doc->setDefaultFont(option.font);
    doc->setHtml(html);
    return doc;
}

} // namespace

void ConnectionFlowDelegate::paint(QPainter *painter,
                                   const QStyleOptionViewItem &option,
                                   const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();

    // Paint the row-spanning gauge background ourselves; suppress the
    // default style's redundant background fill if we did so.
    const bool ownBg = paintGaugeBackground(painter, option, index);
    if (ownBg)
        opt.backgroundBrush = QBrush();

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);

    QTextDocument *doc = buildDoc(opt, index, m_colorCode);
    doc->setTextWidth(textRect.width());

    painter->save();
    painter->translate(textRect.topLeft());
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text,
                         opt.palette.color(opt.state & QStyle::State_Selected
                                               ? QPalette::HighlightedText
                                               : QPalette::Text));
    ctx.clip = QRect(0, 0, textRect.width(), textRect.height());
    doc->documentLayout()->draw(painter, ctx);
    painter->restore();

    delete doc;
}

QSize ConnectionFlowDelegate::sizeHint(const QStyleOptionViewItem &option,
                                       const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QTextDocument *doc = buildDoc(opt, index, m_colorCode);
    doc->setTextWidth(-1);
    const QSize hint = doc->size().toSize();
    delete doc;
    return {hint.width(), hint.height() + 4};
}
