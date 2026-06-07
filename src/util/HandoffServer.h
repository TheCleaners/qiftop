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
// and exports that path via the `QIFTOP_HANDOFF_SOCKET` environment variable
// (the escalator forwards it to the child). The child connects through
// util::HandoffClient and the two processes exchange line-delimited messages.
//
// Wire protocol — one verb per line, optional tab-separated payload:
//   child → parent:
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
    [[nodiscard]] bool    hasChild()    const { return m_client != nullptr; }

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

    QLocalServer *m_server     = nullptr;
    QLocalSocket *m_client     = nullptr;
    QByteArray    m_readBuf;
    QString       m_socketPath;
    QString       m_lastError;
};

// Encodes/decodes the wire format used between HandoffServer and
// HandoffClient. Kept free-function so the child (HandoffClient) and the
// parent (HandoffServer) can share the same routines without circular deps.
namespace handoff {

[[nodiscard]] QByteArray encodeStats(const QList<InterfaceStats> &stats);
[[nodiscard]] QList<InterfaceStats> decodeStats(const QByteArray &payload);

} // namespace handoff
} // namespace util

