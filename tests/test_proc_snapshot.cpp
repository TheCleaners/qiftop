#include <QtTest>

#include "backend/linux/ProcSnapshot.h"

using qiftop::backend::linuximpl::procsnap::parseStartTime;
using qiftop::backend::linuximpl::procsnap::pidStartTime;

class TestProcSnapshot : public QObject
{
    Q_OBJECT
private slots:
    // Typical /proc/<pid>/stat line: starttime is field 22 (1-based).
    void typicalLine()
    {
        const QByteArray s =
            "1234 (bash) S 1 1234 1234 34816 1234 4194304 100 0 0 0 "
            "5 6 0 0 20 0 1 0 987654321 12345678 100 18446744073709551615 "
            "1 1 0 0 0 0 65536 4 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        const auto v = parseStartTime(s);
        QVERIFY(v.has_value());
        QCOMPARE(*v, quint64(987654321));
    }

    // comm containing spaces and parens — must scan from LAST ')'.
    void commWithSpacesAndParens()
    {
        const QByteArray s =
            "42 (weird (cmd) name) R 1 42 42 0 -1 4194304 10 0 0 0 "
            "1 2 0 0 20 0 1 0 1111222233 1024 50 18446744073709551615 "
            "1 1 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        const auto v = parseStartTime(s);
        QVERIFY(v.has_value());
        QCOMPARE(*v, quint64(1111222233));
    }

    void truncatedReturnsNullopt()
    {
        QVERIFY(!parseStartTime(QByteArray("1 (a) S 1 1 1")).has_value());
        QVERIFY(!parseStartTime(QByteArray("garbage no parens here")).has_value());
        QVERIFY(!parseStartTime(QByteArray()).has_value());
    }

    void nonNumericStartTime()
    {
        const QByteArray s =
            "1 (x) S 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 not_a_number 0 "
            "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
        QVERIFY(!parseStartTime(s).has_value());
    }

    // Smoke test against the live system: our own PID must yield a
    // starttime, and a second read must return the SAME value (we
    // haven't been reused).
    void liveSelfIsStable()
    {
        const auto a = pidStartTime(QCoreApplication::applicationPid());
        const auto b = pidStartTime(QCoreApplication::applicationPid());
        QVERIFY(a.has_value());
        QVERIFY(b.has_value());
        QCOMPARE(*a, *b);
    }

    // A definitely-dead pid (PID_MAX_LIMIT is 2^22 by default; we use
    // a value well past any realistic pid) returns nullopt rather than
    // crashing or returning garbage.
    void deadPidReturnsNullopt()
    {
        QVERIFY(!pidStartTime(0x7fffffff).has_value());
    }

    // pid <= 0 is the "unattributed" sentinel everywhere in the
    // resolver chain — it must short-circuit to nullopt, never open
    // /proc/0/stat or /proc/-1/stat.
    void nonPositivePidReturnsNullopt()
    {
        QVERIFY(!pidStartTime(0).has_value());
        QVERIFY(!pidStartTime(-1).has_value());
    }
};

QTEST_MAIN(TestProcSnapshot)
#include "test_proc_snapshot.moc"
