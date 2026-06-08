#pragma once

#include <QObject>
#include <QString>

#include "backend/NetworkMonitor.h"

class QLocalServer;
class QLocalSocket;

namespace util {

// Persistent Unix-domain-socket IPC between the unprivileged parent and the
// privileged child spawned by PrivilegeEscalator.
//
// The parent constructs a HandoffServer, calls listen() to get a socket path,
// and exports both the path and a fresh nonce via the
// `QIFTOP_HANDOFF_SOCKET` and `QIFTOP_HANDOFF_NONCE` environment variables
// (the escalator forwards them to the child). The child connects through
// util::HandoffClient and the two processes exchange line-delimited messages.
//
// SECURITY: The socket is `0600` (`QLocalServer::UserAccessOption`) so only
// the same uid can connect. That's not enough by itself: a hostile same-uid
// process can win the race to connect before the legitimate root child does,
// hijack the channel, and feed forged stats to the tray UI / swallow the
// user's quit/pause clicks. To prevent that the protocol authenticates with
// a per-session nonce: the very first message from the child MUST be
// `HELLO\t<nonce>`; any mismatch (or any other verb before HELLO) results
// in immediate disconnect without dispatch. The nonce is a fresh 256-bit
// random hex string generated per `listen()` and is only readable by
// processes that the parent explicitly forwards the env to.
//
// Wire protocol — one verb per line, optional tab-separated payload:
//   child → parent:
//     HELLO\t<nonce>        (MUST be first) authenticate this socket
//     READY                 child has finished initialising
//     STATS\t<json>         per-interface snapshot (see encodeStats())
//     PAUSED\t{0|1}         child's current pause state
//     BYE                   child is exiting
//   parent → child:
//     SHOW                  raise the privileged main window
//     PAUSE\t{0|1}          set pause state
//     QUIT                  child should exit cleanly
class HandoffServer : public QObject {
    Q_OBJECT

public:
    explicit HandoffServer(QObject *parent = nullptr);
    ~HandoffServer() override;

    QString listen();
    [[nodiscard]] QString errorString() const { return m_lastError; }
    [[nodiscard]] QString socketPath()  const { return m_socketPath; }
    [[nodiscard]] QString nonce()       const { return m_nonce; }
    [[nodiscard]] bool    hasChild()    const { return m_client != nullptr && m_authenticated; }

    void sendShow();
    void sendPause(bool paused);
    void sendQuit();

signals:
    void childReady();
    void childStats(QList<InterfaceStats> stats);
    void childPauseState(bool paused);
    void childDisconnected();

private slots:
    void handleNewConnection();
    void handleReadyRead();
    void handleClientDisconnected();

private:
    void send(const QByteArray &line);
    void dispatch(const QByteArray &line);
    void rejectClient(const char *reason);

    QLocalServer *m_server         = nullptr;
    QLocalSocket *m_client         = nullptr;
    QByteArray    m_readBuf;
    QString       m_socketPath;
    QString       m_nonce;            // 64 hex chars, 256 bits of entropy
    QString       m_lastError;
    bool          m_authenticated   = false;
};

// Encodes/decodes the wire format used between HandoffServer and
// HandoffClient. Kept free-function so the child (HandoffClient) and the
// parent (HandoffServer) can share the same routines without circular deps.
namespace handoff {

[[nodiscard]] QByteArray encodeStats(const QList<InterfaceStats> &stats);
[[nodiscard]] QList<InterfaceStats> decodeStats(const QByteArray &payload);

} // namespace handoff
} // namespace util

