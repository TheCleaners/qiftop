#include "InterfaceNameDelegate.h"
#include "NetworkModel.h"
#include "config/Settings.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPainter>
#include <QTextDocument>

namespace {

// Build a QTextDocument with the appropriate HTML for the cell. The muted
// detail colour is pulled from the option's palette so it tracks the theme.
QTextDocument *buildDoc(const QStyleOptionViewItem &option,
                        const QModelIndex          &index,
                        const Settings             *settings)
{
    const QString name    = index.data(Qt::DisplayRole).toString();
    const QString details = index.data(NetworkModel::DetailsRole).toString();

    const QColor muted = option.palette.color(option.state & QStyle::State_Selected
                                                  ? QPalette::HighlightedText
                                                  : QPalette::PlaceholderText);

    QString html = QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped());
    if (!details.isEmpty()) {
        html += QStringLiteral(" <span style=\"color:%1;\">(%2)</span>")
                    .arg(muted.name(QColor::HexArgb), details.toHtmlEscaped());
    }
    if (settings && settings->trayInterfaces().contains(name)) {
        // U+1F441 EYE — marks rows pinned to the tray summary tooltip.
        html += QStringLiteral(" <span style=\"color:%1;\">&#x1F441;</span>")
                    .arg(muted.name(QColor::HexArgb));
    }

    auto *doc = new QTextDocument;
    doc->setDocumentMargin(0);
    doc->setDefaultFont(option.font);
    doc->setHtml(html);
    return doc;
}

} // namespace

void InterfaceNameDelegate::paint(QPainter *painter,
                                  const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    // Paint the row-spanning gauge backdrop first (base tint + filled
    // overlay); when it draws, suppress the default style's redundant
    // background fill so it doesn't paint over the gauge.
    const bool ownBg = paintGaugeBackground(painter, option, index);

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    // We do the text ourselves; let the style draw background/selection only.
    opt.text.clear();
    if (ownBg)
        opt.backgroundBrush = QBrush();

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText,
                                                 &opt, widget);

    QTextDocument *doc = buildDoc(opt, index, m_settings);
    doc->setTextWidth(textRect.width());

    painter->save();
    painter->translate(textRect.topLeft());
    QAbstractTextDocumentLayout::PaintContext ctx;
    // Default colour for the non-spanned (<b>name</b>) portion.
    ctx.palette.setColor(QPalette::Text,
                         opt.palette.color(opt.state & QStyle::State_Selected
                                               ? QPalette::HighlightedText
                                               : QPalette::Text));
    ctx.clip = QRect(0, 0, textRect.width(), textRect.height());
    doc->documentLayout()->draw(painter, ctx);
    painter->restore();

    delete doc;
}

QSize InterfaceNameDelegate::sizeHint(const QStyleOptionViewItem &option,
                                      const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    QTextDocument *doc = buildDoc(opt, index, m_settings);
    doc->setTextWidth(-1);
    const QSize hint = doc->size().toSize();
    delete doc;

    // Add a small margin so the row doesn't crowd vertically.
    return {hint.width(), hint.height() + 4};
}
