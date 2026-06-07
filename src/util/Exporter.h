#pragma once

#include <QByteArray>
#include <QString>

class Exportable;

namespace util::exporter {

[[nodiscard]] QByteArray toJson(const Exportable &src);
[[nodiscard]] QByteArray toCsv(const Exportable &src);

// Writes data to path atomically (write-then-rename). Returns true on
// success; on failure, *err (if non-null) receives a human-readable reason.
bool save(const QString &path, const QByteArray &data, QString *err = nullptr);

} // namespace util::exporter
