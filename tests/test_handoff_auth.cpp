// Unit tests for util::HandoffServer's nonce authentication path. We
// don't need HandoffClient here — the test acts as a raw QLocalSocket
// peer so we can drive bad protocol behaviour the real client wouldn't.

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include "util/HandoffServer.h"

class TestHandoffAuth : public QObject {
    Q_OBJECT

private:
    static bool waitFor(QLocalSocket &sock, QAbstractSocket::SocketState state,
                        int timeoutMs = 1000)
    {
        QElapsedTimer t; t.start();
        while (int(sock.state()) != int(state)) {
            if (t.elapsed() > timeoutMs) return false;
            QTest::qWait(20);
        }
        return true;
    }

private slots:
    void listenProducesPathAndHexNonce()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY2(!path.isEmpty(), qPrintable(srv.errorString()));
        QCOMPARE(srv.nonce().size(), 64);
        QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
        QVERIFY(hex.match(srv.nonce()).hasMatch());
    }

    void rejectsConnectionWithoutHello()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());
        QSignalSpy ready(&srv, &util::HandoffServer::childReady);

        QLocalSocket sock;
        sock.connectToServer(path);
        QVERIFY(sock.waitForConnected(1000));
        // Send a non-HELLO verb — server MUST disconnect without dispatch.
        sock.write("READY\n");
        sock.flush();
        QVERIFY(waitFor(sock, QAbstractSocket::UnconnectedState));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!srv.hasChild());
    }

    void rejectsHelloWithBadNonce()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());
        QSignalSpy ready(&srv, &util::HandoffServer::childReady);

        QLocalSocket sock;
        sock.connectToServer(path);
        QVERIFY(sock.waitForConnected(1000));
        sock.write("HELLO\tdeadbeef\nREADY\n");
        sock.flush();
        QVERIFY(waitFor(sock, QAbstractSocket::UnconnectedState));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!srv.hasChild());
    }

    void acceptsHelloWithCorrectNonce()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());
        QSignalSpy ready(&srv, &util::HandoffServer::childReady);

        QLocalSocket sock;
        sock.connectToServer(path);
        QVERIFY(sock.waitForConnected(1000));
        const QByteArray line =
            "HELLO\t" + srv.nonce().toLatin1() + "\nREADY\n";
        sock.write(line);
        sock.flush();
        // Allow the server to consume both lines and emit childReady.
        QVERIFY(ready.wait(1000));
        QVERIFY(srv.hasChild());
    }

    void disconnectsOnOversizedPreAuthPayload()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());

        QLocalSocket sock;
        sock.connectToServer(path);
        QVERIFY(sock.waitForConnected(1000));
        // > 1 KiB of unterminated garbage. The server's pre-auth buffer
        // cap should kick in and disconnect us before we ever send '\n'.
        QByteArray spam(2048, 'A');
        sock.write(spam);
        sock.flush();
        QVERIFY(waitFor(sock, QAbstractSocket::UnconnectedState));
    }

    void rejectsSecondConcurrentConnection()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());

        QLocalSocket first;
        first.connectToServer(path);
        QVERIFY(first.waitForConnected(1000));
        // Authenticate the first connection so the slot becomes sticky.
        first.write("HELLO\t" + srv.nonce().toLatin1() + "\n");
        first.flush();
        QTRY_VERIFY_WITH_TIMEOUT(srv.hasChild(), 1000);

        QLocalSocket second;
        second.connectToServer(path);
        QVERIFY(second.waitForConnected(1000));
        // The server already has an *authenticated* peer; the second
        // connection should be dropped immediately.
        QVERIFY(waitFor(second, QAbstractSocket::UnconnectedState));
        // The first peer must still be live.
        QVERIFY(srv.hasChild());
    }

    void evictsUnauthenticatedIncumbentForNewcomer()
    {
        // Defence against pre-auth slot-camping: any same-uid process
        // could otherwise sit on the socket without sending HELLO and
        // block the real privileged child from ever connecting.
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());

        QLocalSocket camper;
        camper.connectToServer(path);
        QVERIFY(camper.waitForConnected(1000));
        // Don't send HELLO — just camp.
        QTest::qWait(50);
        QVERIFY(!srv.hasChild());

        // Newcomer should evict the camper and complete auth.
        QLocalSocket newcomer;
        newcomer.connectToServer(path);
        QVERIFY(newcomer.waitForConnected(1000));
        newcomer.write("HELLO\t" + srv.nonce().toLatin1() + "\n");
        newcomer.flush();
        QTRY_VERIFY_WITH_TIMEOUT(srv.hasChild(), 1000);
        // The camper should have been disconnected by the eviction.
        QTRY_COMPARE_WITH_TIMEOUT(int(camper.state()),
                                  int(QAbstractSocket::UnconnectedState), 1000);
    }

    void noncePersistedAsModeSixHundredFile()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());

        const QString nonceFile = srv.nonceFilePath();
        QVERIFY(!nonceFile.isEmpty());
        QFileInfo fi(nonceFile);
        QVERIFY(fi.exists());
        // POSIX: owner-only RW, no group/world access — otherwise the
        // secret is readable by anyone who can stat the file.
        const auto perms = fi.permissions();
        QVERIFY(perms & QFile::ReadOwner);
        QVERIFY(perms & QFile::WriteOwner);
        QVERIFY(!(perms & QFile::ReadGroup));
        QVERIFY(!(perms & QFile::WriteGroup));
        QVERIFY(!(perms & QFile::ReadOther));
        QVERIFY(!(perms & QFile::WriteOther));

        QFile f(nonceFile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromLatin1(f.readAll().trimmed()), srv.nonce());
    }

    void disconnectsOnOversizedPostAuthPayload()
    {
        util::HandoffServer srv;
        const QString path = srv.listen();
        QVERIFY(!path.isEmpty());

        QLocalSocket sock;
        sock.connectToServer(path);
        QVERIFY(sock.waitForConnected(1000));
        sock.write("HELLO\t" + srv.nonce().toLatin1() + "\n");
        sock.flush();
        QTRY_VERIFY_WITH_TIMEOUT(srv.hasChild(), 1000);

        // Send a >1 MiB unterminated blob post-auth. The cap should kick
        // in and disconnect us before we ever send '\n'.
        QByteArray spam(2 * 1024 * 1024, 'A');
        sock.write(spam);
        sock.flush();
        QVERIFY(waitFor(sock, QAbstractSocket::UnconnectedState, 5000));
    }
};

QTEST_MAIN(TestHandoffAuth)
#include "test_handoff_auth.moc"
