#pragma once

#include "RowGaugeDelegate.h"

class QAbstractItemView;

// Paints the Process and Container columns with a compact secondary
// annotation:
//
//   Process    →  "nginx"   small-text-grey "[1234]"
//   Container  →  "docker:my-app" small-text-grey "▸"  (when chain depth ≥ 2)
//                 "(host)" rendered in italic grey when no container.
//
// Subclasses RowGaugeDelegate so the throughput-gauge backdrop still
// paints when enabled — the attribution columns just get a richer
// foreground than the default styled item.
//
// The pid badge / chain breadcrumb are rendered at ~85% point size in
// the palette's PlaceholderText role, keeping the primary text (comm
// or container name) visually dominant. No icons; we deliberately stay
// font-only so the row height in Flat mode stays identical to v0.1.
class ConnectionAttributionDelegate : public RowGaugeDelegate {
    Q_OBJECT
public:
    explicit ConnectionAttributionDelegate(QAbstractItemView *view,
                                           QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
};
