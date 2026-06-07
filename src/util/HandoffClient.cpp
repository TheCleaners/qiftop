#include "HandoffClient.h"
#include "HandoffServer.h" // for util::handoff::encodeStats
#include "Logging.h"

#include <QLocalSocket>

namespace util {

HandoffClient::HandoffClient(QObject *parent)
    : QObject(parent)
{}

HandoffClient::~HandoffClient()
{
    if (m_sock && m_sock->state() == QLocalSocket::ConnectedState) {
        sendBye();
        m_sock->disconnectFromServer();
        m_sock->waitForDisconnected(500);
    }
}

bool HandoffClient::connectTo(const QString &socketPath, int timeoutMs)
{
    if (socketPath.isEmpty()) return false;
    if (m_sock) return m_sock->state() == QLocalSocket::ConnectedState;

    m_sock = new QLocalSocket(this);
    connect(m_sock, &QLocalSocket::readyRead,    this, &HandoffClient::handleReadyRead);
    connect(m_sock, &QLocalSocket::disconnected, this, &HandoffClient::handleDisconnected);
    m_sock->connectToServer(socketPath);
    if (!m_sock->waitForConnected(timeoutMs)) {
        qCWarning(lcVerbose).noquote()
            << "handoff-client: connect failed:" << m_sock->errorString();
        m_sock->deleteLater();
        m_sock = nullptr;
        return false;
    }
    qCInfo(lcVerbose).noquote() << "handoff-client: connected to" << socketPath;
    return true;
}

bool HandoffClient::isConnected() const
{
    return m_sock && m_sock->state() == QLocalSocket::ConnectedState;
}

void HandoffClient::handleReadyRead()
{
    if (!m_sock) return;
    m_readBuf += m_sock->readAll();
    int nl;
    while ((nl = m_readBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_readBuf.left(nl);
        m_readBuf.remove(0, nl + 1);
        dispatch(line);
    }
}

void HandoffClient::handleDisconnected()
{
    qCInfo(lcVerbose) << "handoff-client: parent disconnected";
    m_readBuf.clear();
    emit disconnected();
}

void HandoffClient::dispatch(const QByteArray &line)
{
    if (line.isEmpty()) return;
    const int tab = line.indexOf('\t');
    const QByteArray verb    = tab < 0 ? line : line.left(tab);
    const QByteArray payload = tab < 0 ? QByteArray() : line.mid(tab + 1);

    qCDebug(lcVerbose).noquote() << "handoff-client <-" << verb;

    if (verb == "SHOW") {
        emit showRequested();
    } else if (verb == "PAUSE") {
        emit pauseCommand(payload.trimmed() == "1");
    } else if (verb == "QUIT") {
        emit quitCommand();
    }
}

void HandoffClient::send(const QByteArray &line)
{
    if (!m_sock || m_sock->state() != QLocalSocket::ConnectedState) return;
    m_sock->write(line);
    m_sock->write("\n");
    m_sock->flush();
}

void HandoffClient::sendReady() { send("READY"); }

void HandoffClient::sendStats(const QList<InterfaceStats> &stats)
{
    QByteArray line = "STATS\t";
    line += handoff::encodeStats(stats);
    send(line);
}

void HandoffClient::sendPauseState(bool paused)
{
    send(QByteArray("PAUSED\t") + (paused ? "1" : "0"));
}

void HandoffClient::sendBye() { send("BYE"); }

} // namespace util
