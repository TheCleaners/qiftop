#pragma once

#include <QList>
#include <QString>
#include <QVariant>

// Lightweight interface implemented by models that can be serialised to
// JSON/CSV via util::exporter. Intentionally not a QObject so it can be
// mixed into QAbstractItemModel subclasses freely.
class Exportable {
public:
    virtual ~Exportable() = default;

    // Column titles, in display order. Used as CSV headers and JSON keys.
    [[nodiscard]] virtual QStringList exportHeaders() const = 0;

    [[nodiscard]] virtual int exportRowCount() const = 0;

    // Values for the given row, matching exportHeaders() one-for-one.
    // Numeric columns SHOULD return real numbers (not formatted strings)
    // so that downstream tools can analyse them directly.
    [[nodiscard]] virtual QVariantList exportRow(int row) const = 0;
};
