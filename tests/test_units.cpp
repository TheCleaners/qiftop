// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for util::formatBytes / util::formatByteRate (src/util/Units.h).
// Pure formatters; small surface area. The point is to nail down unit
// boundaries (1024-based, IEC suffixes) and decimal precision so a
// well-meaning edit to "use SI" or "always show two decimals" gets caught
// immediately.

#include <QtTest/QtTest>

#include "util/Units.h"

class TestUnits : public QObject {
    Q_OBJECT

private slots:
    void formatBytes_data();
    void formatBytes();
    void formatByteRate_data();
    void formatByteRate();
    void formatByteRate_zero();
    void formatByteRate_subKiB();
    void formatBytes_iecNotSi();
};

void TestUnits::formatBytes_data()
{
    QTest::addColumn<quint64>("bytes");
    QTest::addColumn<QString>("expected");

    QTest::newRow("zero")      << quint64(0)                       << QStringLiteral("0 B");
    QTest::newRow("1023B")     << quint64(1023)                    << QStringLiteral("1023 B");
    QTest::newRow("1KiB")      << quint64(1024)                    << QStringLiteral("1.0 KiB");
    QTest::newRow("1.5KiB")    << quint64(1024 + 512)              << QStringLiteral("1.5 KiB");
    QTest::newRow("1MiB-1")    << quint64(1024ULL * 1024 - 1)      << QStringLiteral("1024.0 KiB");
    QTest::newRow("1MiB")      << quint64(1024ULL * 1024)          << QStringLiteral("1.00 MiB");
    QTest::newRow("1GiB")      << quint64(1024ULL * 1024 * 1024)   << QStringLiteral("1.00 GiB");
    QTest::newRow("1TiB")      << quint64(1024ULL * 1024 * 1024 * 1024) << QStringLiteral("1.00 TiB");
    QTest::newRow("2.5GiB")    << quint64(2560ULL * 1024 * 1024)   << QStringLiteral("2.50 GiB");
}

void TestUnits::formatBytes()
{
    QFETCH(quint64, bytes);
    QFETCH(QString, expected);
    QCOMPARE(util::formatBytes(bytes), expected);
}

void TestUnits::formatByteRate_data()
{
    QTest::addColumn<double>("rate");
    QTest::addColumn<QString>("expected");

    QTest::newRow("zero")         << 0.0                       << QStringLiteral("0 B/s");
    QTest::newRow("500B/s")       << 500.0                     << QStringLiteral("500 B/s");
    QTest::newRow("1023B/s")      << 1023.0                    << QStringLiteral("1023 B/s");
    QTest::newRow("1KiB/s")       << 1024.0                    << QStringLiteral("1.0 KiB/s");
    QTest::newRow("1MiB/s")       << 1024.0 * 1024.0           << QStringLiteral("1.00 MiB/s");
    QTest::newRow("1GiB/s")       << 1024.0 * 1024.0 * 1024.0  << QStringLiteral("1.00 GiB/s");
    QTest::newRow("2.5MiB/s")     << 2.5 * 1024.0 * 1024.0     << QStringLiteral("2.50 MiB/s");
}

void TestUnits::formatByteRate()
{
    QFETCH(double, rate);
    QFETCH(QString, expected);
    QCOMPARE(util::formatByteRate(rate), expected);
}

void TestUnits::formatByteRate_zero()
{
    // Regression: must not divide by zero or produce "nan/inf"
    QCOMPARE(util::formatByteRate(0.0), QStringLiteral("0 B/s"));
    QVERIFY(!util::formatByteRate(0.0).contains(QStringLiteral("nan"), Qt::CaseInsensitive));
}

void TestUnits::formatByteRate_subKiB()
{
    // Fractional B/s gets truncated to integer (current behaviour — pin it
    // so future floating-point edits don't accidentally start showing
    // "0.5 B/s" which would look broken next to whole-byte counts).
    QCOMPARE(util::formatByteRate(42.7), QStringLiteral("42 B/s"));
}

void TestUnits::formatBytes_iecNotSi()
{
    // Regression: 1000 must NOT be promoted to KiB (we use 1024-based IEC,
    // not 1000-based SI). A careless edit to "use kB at 1000" would change
    // behaviour silently across the whole UI.
    QCOMPARE(util::formatBytes(1000), QStringLiteral("1000 B"));
    QCOMPARE(util::formatBytes(1023), QStringLiteral("1023 B"));
}

QTEST_APPLESS_MAIN(TestUnits)
#include "test_units.moc"
