// Tests for backend::linux_::readProcessDetails. Drives the helper
// against /proc/self/ (always reachable, always readable to the test
// user) so we don't need root or fixture-symlink trickery. PID
// recovery via getpid() lets us assert exact equality with the
// caller, and starttime > 0 acts as a smoke test for the /proc/<pid>/stat
// field-22 parser (which has to handle (comm) values containing spaces
// and parens).

#include <QTest>

#include "backend/linux/ProcDetails.h"

#include <unistd.h>

using qiftop::backend::linux_::readProcessDetails;

class TestProcDetails : public QObject {
    Q_OBJECT
private slots:
    void invalidPidYieldsInvalid()
    {
        const auto d = readProcessDetails(0);
        QVERIFY(!d.valid);
        QCOMPARE(d.pid, qint32(0));

        const auto d2 = readProcessDetails(-1);
        QVERIFY(!d2.valid);
    }

    void unreachablePidYieldsInvalidNotCrash()
    {
        // PID_MAX on Linux defaults to 4194304; passing in a value
        // beyond that guarantees no such process. Must not throw.
        const auto d = readProcessDetails(2147483646);
        QVERIFY(!d.valid);
        QCOMPARE(d.pid, qint32(0));
        QVERIFY(d.cmdline.isEmpty());
        QVERIFY(d.exe.isEmpty());
    }

    void selfPidRoundTrips()
    {
        const auto d = readProcessDetails(::getpid());
        QVERIFY2(d.valid, qPrintable(QStringLiteral("our own PID %1 must be reachable")
                                         .arg(::getpid())));
        QCOMPARE(d.pid, qint32(::getpid()));
        QVERIFY(!d.comm.isEmpty());
        // cmdline must contain our binary basename.
        QVERIFY2(d.cmdline.contains(QStringLiteral("test_proc_details")),
                 qPrintable(d.cmdline));
        QVERIFY(!d.exe.isEmpty());
        // cwd should be readable (we're our own UID).
        QVERIFY(!d.cwd.isEmpty());
        // startTime in jiffies — must be > 0 for any process that has
        // been alive longer than a jiffy. Pinning this catches the
        // off-by-one in /proc/<pid>/stat parsing (field index 19 vs 22).
        QVERIFY2(d.startTimeJiffies > 0,
                 qPrintable(QStringLiteral("startTimeJiffies=%1")
                                .arg(d.startTimeJiffies)));
    }

    void uidMatchesRealUid()
    {
        const auto d = readProcessDetails(::getpid());
        QCOMPARE(d.uid, quint32(::getuid()));
    }

    void honoursAlternateProcRoot()
    {
        // Defaults flow through /proc; we don't have a full fixture
        // tree, but pointing at a non-existent root must yield invalid
        // without crashing. This pins the path-parameter plumbing.
        const auto d = readProcessDetails(::getpid(),
            QStringLiteral("/this/does/not/exist"));
        QVERIFY(!d.valid);
    }
};

QTEST_GUILESS_MAIN(TestProcDetails)
#include "test_proc_details.moc"
