#include "Exporter.h"
#include "Exportable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStringList>

namespace util::exporter {

namespace {

QJsonValue toJsonValue(const QVariant &v)
{
    switch (static_cast<QMetaType::Type>(v.typeId())) {
    case QMetaType::Bool:        return v.toBool();
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:    return v.toLongLong();
    // QJsonValue stores numbers as double, so anything past 2^53 silently
    // loses precision. Cumulative interface counters on a long-lived
    // 100 Gb/s NIC (or aggregated conntrack byte totals on a busy router)
    // can exceed that. Encode as a decimal string so consumers that care
    // about exact values can recover them; consumers that don't care can
    // still parse the digits back into a double.
    case QMetaType::ULongLong:   return QString::number(v.toULongLong());
    case QMetaType::Double:
    case QMetaType::Float:       return v.toDouble();
    case QMetaType::QStringList: {
        QJsonArray arr;
        for (const QString &s : v.toStringList())
            arr.append(s);
        return arr;
    }
    default:
        return QJsonValue::fromVariant(v);
    }
}

// Neutralise CSV-injection ("formula injection") payloads. A field that
// begins with one of these characters will be interpreted as a formula by
// Excel / LibreOffice / Google Sheets, allowing an attacker who controls
// the input (notably reverse-DNS hostnames, but also interface names from
// the kernel) to execute spreadsheet formulas in the consumer's session
// when the exported CSV is opened.
//
// The fix prepends a single leading apostrophe, which spreadsheet apps
// uniformly treat as a literal-text marker. We do it before the quoting
// step so the apostrophe ends up *inside* the quoted field if quoting is
// also needed.
static QString csvSanitise(const QString &field)
{
    if (field.isEmpty()) return field;
    const QChar c0 = field.at(0);
    if (c0 == QLatin1Char('=') || c0 == QLatin1Char('+') ||
        c0 == QLatin1Char('-') || c0 == QLatin1Char('@') ||
        c0 == QLatin1Char('\t') || c0 == QLatin1Char('\r')) {
        return QLatin1Char('\'') + field;
    }
    return field;
}

QString csvEscape(const QString &raw)
{
    const QString field = csvSanitise(raw);
    const bool needsQuoting = field.contains(QLatin1Char(','))
                              || field.contains(QLatin1Char('"'))
                              || field.contains(QLatin1Char('\n'))
                              || field.contains(QLatin1Char('\r'));
    if (!needsQuoting)
        return field;
    QString escaped = field;
    escaped.replace(QLatin1Char('"'), QLatin1String("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString csvFlatten(const QVariant &v)
{
    if (v.typeId() == QMetaType::QStringList)
        return v.toStringList().join(QLatin1Char(' '));
    return v.toString();
}

} // namespace

QByteArray toJson(const Exportable &src)
{
    const QStringList headers = src.exportHeaders();
    QJsonArray rows;
    for (int i = 0, n = src.exportRowCount(); i < n; ++i) {
        const QVariantList values = src.exportRow(i);
        QJsonObject obj;
        for (int c = 0; c < headers.size() && c < values.size(); ++c)
            obj.insert(headers[c], toJsonValue(values[c]));
        rows.append(obj);
    }
    return QJsonDocument(rows).toJson(QJsonDocument::Indented);
}

QByteArray toCsv(const Exportable &src)
{
    const QStringList headers = src.exportHeaders();

    QStringList lines;
    {
        QStringList row;
        row.reserve(headers.size());
        for (const QString &h : headers)
            row << csvEscape(h);
        lines << row.join(QLatin1Char(','));
    }

    for (int i = 0, n = src.exportRowCount(); i < n; ++i) {
        const QVariantList values = src.exportRow(i);
        QStringList row;
        row.reserve(values.size());
        for (const QVariant &v : values)
            row << csvEscape(csvFlatten(v));
        lines << row.join(QLatin1Char(','));
    }

    return (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
}

bool save(const QString &path, const QByteArray &data, QString *err)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (err) *err = f.errorString();
        return false;
    }
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

} // namespace util::exporter
