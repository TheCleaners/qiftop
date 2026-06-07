#pragma once

#include "RowGaugeDelegate.h"

// Renders the Flow column for connections:
//
//     [PROTO]  source:port → destination:port
//
// When color-coding is enabled (the default), each part is tinted: the
// proto tag stays muted, source is "cool" (blue/cyan), destination is
// "warm" (orange/amber). With color-coding disabled, everything uses
// the palette's Text color (proto + arrow still muted) for a calmer
// look. The proto tag itself is always rendered in upright (non-italic)
// type even when the row is stale, so a single consistent typographic
// label identifies the protocol regardless of row state.
//
// Inherits from RowGaugeDelegate so the row-spanning throughput gauge
// background is painted under the rich-text content.
class ConnectionFlowDelegate : public RowGaugeDelegate {
    Q_OBJECT

public:
    using RowGaugeDelegate::RowGaugeDelegate;

    void setColorCodeEnabled(bool v) { m_colorCode = v; }
    [[nodiscard]] bool colorCodeEnabled() const { return m_colorCode; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override;

private:
    bool m_colorCode = true;
};
