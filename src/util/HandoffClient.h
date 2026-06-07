#pragma once

#include <QObject>

#include "backend/NetworkMonitor.h"

class QLocalSocket;

namespace util {

// Child-side counterpart to HandoffServer. The privileged child constructs
// this, calls connectTo(path) (the path is normally in
// `$QIFTOP_HANDOFF_SOCKET`) and then uses sendReady() once its UI is up,
// sendStats() periodically, and the *Requested signals to react to user
// actions performed on the parent's tray icon.
class HandoffClient : public QObject {
    Q_OBJECT

public:
    explicit HandoffClient(QObject *parent = nullptr);
    ~HandoffClient() override;

    // Establishes the connection synchronously (waits up to timeoutMs for
    // QLocalSocket to enter Connected state). Returns false on failure.
    bool connectTo(const QString &socketPath, int timeoutMs = 2000);

    [[nodiscard]] bool isConnected() const;

    void sendReady();
    void sendStats(const QList<InterfaceStats> &stats);
    void sendPauseState(bool paused);
    void sendBye();

signals:
    void showRequested();
    void pauseCommand(bool paused);
    void quitCommand();
    void disconnected();

private slots:
    void handleReadyRead();
    void handleDisconnected();

private:
    void send(const QByteArray &line);
    void dispatch(const QByteArray &line);

    QLocalSocket *m_sock = nullptr;
    QByteArray    m_readBuf;
};

} // namespace util
