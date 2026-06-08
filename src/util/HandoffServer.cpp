#include "HandoffServer.h"
#include "Logging.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUuid>

namespace util {

namespace {
// Generate 256 bits of cryptographic-grade randomness rendered as 64 hex
// chars. QRandomGenerator::system() is the OS CSPRNG (getrandom / /dev/urandom
// on Linux), which is what we want — we are NOT looking for reproducibility.
QString makeNonce()
{
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words);
    QString out;
    out.reserve(64);
    for (quint32 w : words)
        out += QStringLiteral("%1").arg(w, 8, 16, QLatin1Char('0'));
    return out;
}
} // namespace

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

    m_nonce = makeNonce();

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        m_lastError  = m_server->errorString();
        m_socketPath.clear();
        m_nonce.clear();
        delete m_server;
        m_server = nullptr;
        return {};
    }

    connect(m_server, &QLocalServer::newConnection,
            this,     &HandoffServer::handleNewConnection);

    qCInfo(lcVerbose).noquote() << "handoff: listening on" << m_socketPath
                                << "(nonce: 64 hex chars)";
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
        m_authenticated = false;
        connect(sock, &QLocalSocket::readyRead,    this, &HandoffServer::handleReadyRead);
        connect(sock, &QLocalSocket::disconnected, this, &HandoffServer::handleClientDisconnected);
        qCInfo(lcVerbose) << "handoff: peer connected (awaiting HELLO)";
        // Drain anything already buffered.
        if (sock->bytesAvailable() > 0)
            handleReadyRead();
    }
}

void HandoffServer::rejectClient(const char *reason)
{
    qCWarning(lcVerbose).noquote() << "handoff: rejecting peer:" << reason;
    if (m_client) {
        m_client->disconnectFromServer();
        // handleClientDisconnected() will clean up.
    }
    m_readBuf.clear();
}

void HandoffServer::handleReadyRead()
{
    if (!m_client) return;
    m_readBuf += m_client->readAll();
    // Cap pre-auth buffer so a non-cooperative peer can't OOM us by sending
    // a giant unterminated line before HELLO.
    if (!m_authenticated && m_readBuf.size() > 1024) {
        rejectClient("oversized pre-auth payload");
        return;
    }
    int nl;
    while ((nl = m_readBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_readBuf.left(nl);
        m_readBuf.remove(0, nl + 1);
        if (!m_authenticated) {
            // Very first line MUST be `HELLO\t<nonce>`. Anything else =
            // hostile or buggy peer; disconnect without dispatch.
            static constexpr char kHello[] = "HELLO\t";
            if (!line.startsWith(kHello)) {
                rejectClient("missing HELLO");
                return;
            }
            const QByteArray gotNonce = line.mid(sizeof(kHello) - 1).trimmed();
            // Constant-time-ish compare via QByteArray::operator==; not a
            // real timing-channel concern here (single LAN socket, no
            // remote attacker, no oracle) but cheap to do right.
            if (gotNonce != m_nonce.toLatin1()) {
                rejectClient("nonce mismatch");
                return;
            }
            m_authenticated = true;
            qCInfo(lcVerbose) << "handoff: child authenticated";
            continue;
        }
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
    m_authenticated = false;
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
    // Refuse to send commands to an unauthenticated peer.
    if (!m_client || !m_authenticated) return;
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

