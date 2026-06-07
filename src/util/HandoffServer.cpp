#include "HandoffServer.h"
#include "Logging.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QUuid>

namespace util {

HandoffServer::HandoffServer(QObject *parent)
    : QObject(parent)
{}

HandoffServer::~HandoffServer()
{
    if (m_client) {
        m_client->disconnectFromServer();
    }
    if (m_server) {
        m_server->close();
    }
    if (!m_socketPath.isEmpty()) {
        QFile::remove(m_socketPath);
    }
}

QString HandoffServer::listen()
{
    if (m_server) return m_socketPath;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QStringLiteral("/tmp");

    const QString name = QStringLiteral("qiftop-handoff-%1.sock")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_socketPath = QDir(dir).filePath(name);
    QFile::remove(m_socketPath);

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        m_lastError  = m_server->errorString();
        m_socketPath.clear();
        delete m_server;
        m_server = nullptr;
        return {};
    }

    connect(m_server, &QLocalServer::newConnection,
            this,     &HandoffServer::handleNewConnection);

    qCInfo(lcVerbose).noquote() << "handoff: listening on" << m_socketPath;
    return m_socketPath;
}

void HandoffServer::handleNewConnection()
{
    while (auto *sock = m_server->nextPendingConnection()) {
        // Only accept one persistent client — the privileged child. Reject
        // any further connections so a stray process can't hijack us.
        if (m_client) {
            qCInfo(lcVerbose) << "handoff: rejecting extra connection";
            sock->disconnectFromServer();
            sock->deleteLater();
            continue;
        }
        m_client = sock;
        connect(sock, &QLocalSocket::readyRead,    this, &HandoffServer::handleReadyRead);
        connect(sock, &QLocalSocket::disconnected, this, &HandoffServer::handleClientDisconnected);
        qCInfo(lcVerbose) << "handoff: child connected";
        // Drain anything already buffered.
        if (sock->bytesAvailable() > 0)
            handleReadyRead();
    }
}

void HandoffServer::handleReadyRead()
{
    if (!m_client) return;
    m_readBuf += m_client->readAll();
    int nl;
    while ((nl = m_readBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_readBuf.left(nl);
        m_readBuf.remove(0, nl + 1);
        dispatch(line);
    }
}

void HandoffServer::handleClientDisconnected()
{
    qCInfo(lcVerbose) << "handoff: child disconnected";
    if (m_client) {
        m_client->deleteLater();
        m_client = nullptr;
    }
    m_readBuf.clear();
    emit childDisconnected();
}

void HandoffServer::dispatch(const QByteArray &line)
{
    if (line.isEmpty()) return;
    const int tab = line.indexOf('\t');
    const QByteArray verb    = tab < 0 ? line : line.left(tab);
    const QByteArray payload = tab < 0 ? QByteArray() : line.mid(tab + 1);

    qCDebug(lcVerbose).noquote() << "handoff <-" << verb
                                 << "(" << payload.size() << "B)";

    if (verb == "READY") {
        emit childReady();
    } else if (verb == "STATS") {
        emit childStats(handoff::decodeStats(payload));
    } else if (verb == "PAUSED") {
        emit childPauseState(payload.trimmed() == "1");
    } else if (verb == "BYE") {
        // Child is shutting down; treat as disconnect so the parent can quit.
        if (m_client)
            m_client->disconnectFromServer();
    }
}

void HandoffServer::send(const QByteArray &line)
{
    if (!m_client) return;
    m_client->write(line);
    m_client->write("\n");
    m_client->flush();
}

void HandoffServer::sendShow()                 { send("SHOW"); }
void HandoffServer::sendPause(bool paused)     { send(QByteArray("PAUSE\t") + (paused ? "1" : "0")); }
void HandoffServer::sendQuit()                 { send("QUIT"); }

namespace handoff {

QByteArray encodeStats(const QList<InterfaceStats> &stats)
{
    QJsonArray arr;
    for (const InterfaceStats &s : stats) {
        QJsonObject o;
        o.insert(QStringLiteral("n"), s.name);
        o.insert(QStringLiteral("r"), double(s.rxBytes));
        o.insert(QStringLiteral("t"), double(s.txBytes));
        o.insert(QStringLiteral("l"), s.isLoopback);
        o.insert(QStringLiteral("u"), s.isUp);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("items"), arr);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QList<InterfaceStats> decodeStats(const QByteArray &payload)
{
    QList<InterfaceStats> out;
    const auto doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) return out;
    const auto arr = doc.object().value(QStringLiteral("items")).toArray();
    out.reserve(arr.size());
    for (const auto &v : arr) {
        const auto o = v.toObject();
        InterfaceStats s;
        s.name       = o.value(QStringLiteral("n")).toString();
        s.rxBytes    = quint64(o.value(QStringLiteral("r")).toDouble());
        s.txBytes    = quint64(o.value(QStringLiteral("t")).toDouble());
        s.isLoopback = o.value(QStringLiteral("l")).toBool();
        s.isUp       = o.value(QStringLiteral("u")).toBool();
        out.append(s);
    }
    return out;
}

} // namespace handoff
} // namespace util

