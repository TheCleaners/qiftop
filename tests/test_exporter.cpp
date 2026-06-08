// Unit tests for util::exporter — focuses on numerical fidelity (the
// reason quint64 is encoded as a decimal string in JSON) and CSV
// formula-injection neutralisation. Plain ints/strings/bools just go
// through QJson's defaults, so they're covered implicitly.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTest>

#include "util/Exportable.h"
#include "util/Exporter.h"

namespace {

class FakeRows : public Exportable {
public:
    QStringList   headers;
    QList<QVariantList> rows;

    [[nodiscard]] QStringList exportHeaders() const override { return headers; }
    [[nodiscard]] int         exportRowCount() const override { return rows.size(); }
    [[nodiscard]] QVariantList exportRow(int r) const override { return rows.at(r); }
};

} // namespace

class TestExporter : public QObject {
    Q_OBJECT

private slots:
    void quint64IsEncodedAsStringPreservingPrecision()
    {
        // 2^60 — well past the 2^53 precision boundary of an IEEE-754 double,
        // which is what QJsonValue uses internally for numbers. If we round-
        // tripped through QJsonValue::Double we'd lose the lower bits.
        const quint64 big = 1ULL << 60;
        FakeRows src;
        src.headers = {QStringLiteral("bytes")};
        src.rows.append({QVariant::fromValue<quint64>(big)});

        const QByteArray json = util::exporter::toJson(src);
        const QJsonArray  arr = QJsonDocument::fromJson(json).array();
        QCOMPARE(arr.size(), 1);
        const QJsonValue v = arr.first().toObject().value(QStringLiteral("bytes"));

        QVERIFY2(v.isString(),
                 "ULongLong must be encoded as a JSON string so values past "
                 "2^53 keep their exact decimal representation");
        QCOMPARE(v.toString(), QString::number(big));
    }

    void signedLongLongStaysNumeric()
    {
        // qint64 (e.g. negative deltas, signed counters) should remain a
        // JSON number — only ULongLong gets the string treatment, since
        // negative values can't represent the >2^53 unsigned range anyway.
        FakeRows src;
        src.headers = {QStringLiteral("n")};
        src.rows.append({QVariant::fromValue<qint64>(-12345)});

        const QJsonArray arr = QJsonDocument::fromJson(
            util::exporter::toJson(src)).array();
        const QJsonValue v = arr.first().toObject().value(QStringLiteral("n"));
        QVERIFY(v.isDouble());
        QCOMPARE(v.toVariant().toLongLong(), qint64{-12345});
    }

    void csvNeutralisesFormulaInjection()
    {
        // Reverse-DNS hostnames and (in theory) kernel-provided interface
        // names can begin with =/+/-/@/tab/CR/LF, which spreadsheet apps
        // interpret as formulas. The exporter must defang those by
        // prepending a literal apostrophe before any quoting takes place.
        FakeRows src;
        src.headers = {QStringLiteral("host")};
        src.rows.append({QStringLiteral("=cmd|'/c calc'!A1")});

        const QByteArray csv = util::exporter::toCsv(src);
        // Find the data row (skip header line). Should start with '"'\''=...'
        const QList<QByteArray> lines = csv.split('\n');
        QVERIFY(lines.size() >= 2);
        const QByteArray data = lines.at(1);
        QVERIFY2(data.contains("'="),
                 "leading '=' must be escaped with a leading apostrophe");
        QVERIFY2(!data.startsWith('='),
                 "raw '=' must not be emitted at the start of a CSV field");
    }
};

QTEST_MAIN(TestExporter)
#include "test_exporter.moc"
