#pragma once

#include <QtGlobal>

namespace qiftop::ui {

// Shared item-data roles for the row-spanning bandwidth gauge painted by
// RowGaugeDelegate. Both ConnectionModel and NetworkModel expose them so the
// single delegate can paint either view's rows. The offset is deliberately
// high so it never collides with a model's own UserRole-based role block.
inline constexpr int GaugeFractionRole  = Qt::UserRole + 800; // double in [0,1]
inline constexpr int GaugeDarkColorRole = Qt::UserRole + 801; // QColor (filled portion)

} // namespace qiftop::ui
