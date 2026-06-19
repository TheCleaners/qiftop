#pragma once

#include "RowGaugeDelegate.h"

class Settings;

// Paints the Interface column as a bold name followed by a parenthesised
// detail string in the palette's "placeholder" colour, so it stays readable
// on both light and dark themes.
//
// The plain ifname stays in Qt::DisplayRole (used for sorting, accessibility,
// and the editor); the details come from NetworkModel::DetailsRole. When the
// interface is currently part of the tray-summary set, an eye glyph is
// appended so users can see at a glance which rows are pinned.
//
// Inherits RowGaugeDelegate so the row-spanning bandwidth gauge background is
// painted under the name cell too (parity with the nqiftop interface bars).
class InterfaceNameDelegate : public RowGaugeDelegate {
    Q_OBJECT

public:
    using RowGaugeDelegate::RowGaugeDelegate;

    void setSettings(Settings *settings) { m_settings = settings; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override;

private:
    Settings *m_settings = nullptr;
};
