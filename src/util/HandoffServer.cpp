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
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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
    if (!m_nonceFilePath.isEmpty()) {
        QFile::remove(m_nonceFilePath);
    }
    if (!m_socketDir.isEmpty()) {
        // Best-effort rmdir; only succeeds if empty, which is what we want
        // (don't take out user data if something unexpected lived here).
        QDir().rmdir(m_socketDir);
    }
}

namespace {
// Return a per-user runtime directory suitable for our 0600 socket + nonce.
// Prefers $XDG_RUNTIME_DIR (kernel-managed, mode 0700, tmpfs); falls back
// to a freshly mkdtemp'd 0700 directory under $HOME/.cache/qiftop/. Never
// returns /tmp: bind()-then-chmod in a world-writable directory leaves a
// permission-race window during which any same-host user can connect.
QString pickRuntimeDir(QString *outErr)
{
    const QString xdg = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty() && QDir(xdg).exists())
        return xdg;
    const QString cacheRoot =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (cacheRoot.isEmpty()) {
        if (outErr) *outErr = QStringLiteral("no writable cache directory");
        return {};
    }
    QDir().mkpath(cacheRoot + QStringLiteral("/qiftop"));
    QTemporaryDir tdir(cacheRoot + QStringLiteral("/qiftop/handoff-XXXXXX"));
    if (!tdir.isValid()) {
        if (outErr) *outErr = tdir.errorString();
        return {};
    }
    tdir.setAutoRemove(false); // caller cleans up
    // QTemporaryDir creates with 0700 on POSIX.
    return tdir.path();
}
} // namespace

QString HandoffServer::listen()
{
    if (m_server) return m_socketPath;

    QString pickErr;
    QString dir = pickRuntimeDir(&pickErr);
    if (dir.isEmpty()) {
        m_lastError = QStringLiteral("handoff: no safe runtime dir: %1").arg(pickErr);
        return {};
    }
    // If pickRuntimeDir created a fresh per-handoff directory under
    // ~/.cache/qiftop/, remember it so we can rmdir on teardown.
    if (!dir.startsWith(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
        || QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation).isEmpty())
    {
        m_socketDir = dir;
    }

    const QString name = QStringLiteral("qiftop-handoff-%1.sock")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_socketPath = QDir(dir).filePath(name);
    QFile::remove(m_socketPath);

    m_nonce = makeNonce();

    // Persist the nonce to a 0600 file rather than passing it on argv: argv
    // is world-readable via /proc/<pid>/cmdline during the (often long)
    // pkexec auth prompt window. The child reads it via HandoffClient and
    // unlinks immediately.
    m_nonceFilePath = QDir(dir).filePath(
        QStringLiteral("qiftop-handoff-%1.nonce")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    {
        QSaveFile nf(m_nonceFilePath);
        // Create with restrictive perms; QSaveFile honours setPermissions
        // before the atomic rename.
        if (!nf.open(QIODevice::WriteOnly)) {
            m_lastError = QStringLiteral("handoff: cannot write nonce file: %1")
                              .arg(nf.errorString());
            m_socketPath.clear();
            m_nonce.clear();
            m_nonceFilePath.clear();
            return {};
        }
        nf.write(m_nonce.toLatin1());
        if (!nf.commit()) {
            m_lastError = QStringLiteral("handoff: cannot commit nonce file: %1")
                              .arg(nf.errorString());
            m_socketPath.clear();
            m_nonce.clear();
            m_nonceFilePath.clear();
            return {};
        }
        QFile::setPermissions(m_nonceFilePath,
                              QFile::ReadOwner | QFile::WriteOwner);
    }
    m_expectedChildUid = ::geteuid();

    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        m_lastError  = m_server->errorString();
        QFile::remove(m_nonceFilePath);
        m_socketPath.clear();
        m_nonce.clear();
        m_nonceFilePath.clear();
        delete m_server;
        m_server = nullptr;
        return {};
    }

    connect(m_server, &QLocalServer::newConnection,
            this,     &HandoffServer::handleNewConnection);

    qCInfo(lcVerbose).noquote() << "handoff: listening on" << m_socketPath
                                << "(nonce file:" << m_nonceFilePath << ")";
    return m_socketPath;
}

void HandoffServer::handleNewConnection()
{
    while (auto *sock = m_server->nextPendingConnection()) {
        // SO_PEERCRED gate: the legitimate child is either the same uid
        // (self-elevation handoff via pkexec → root → drops back to user)
        // or root (privileged child writing back). Reject any other uid
        // before they even get a chance to send HELLO. This is defence in
        // depth — UserAccessOption already 0600s the socket — but cheap.
        const qintptr fd = sock->socketDescriptor();
        struct ucred cred{};
        socklen_t len = sizeof(cred);
        if (fd < 0 || getsockopt(int(fd), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
            qCWarning(lcVerbose) << "handoff: SO_PEERCRED failed; rejecting peer";
            sock->disconnectFromServer();
            sock->deleteLater();
            continue;
        }
        if (cred.uid != m_expectedChildUid && cred.uid != 0) {
            qCWarning(lcVerbose).noquote()
                << "handoff: rejecting peer uid" << cred.uid
                << "(expected" << m_expectedChildUid << "or 0)";
            sock->disconnectFromServer();
            sock->deleteLater();
            continue;
        }

        if (m_client) {
            // If the incumbent is unauthenticated, evict it: an attacker
            // could otherwise camp the slot pre-auth and lock out the real
            // root child forever. Once authenticated, the slot is sticky.
            if (!m_authenticated) {
                qCInfo(lcVerbose)
                    << "handoff: evicting unauthenticated incumbent peer "
                       "to make room for newcomer";
                disconnect(m_client, nullptr, this, nullptr);
                m_client->disconnectFromServer();
                m_client->deleteLater();
                m_client = nullptr;
                m_readBuf.clear();
            } else {
                qCInfo(lcVerbose) << "handoff: rejecting extra connection";
                sock->disconnectFromServer();
                sock->deleteLater();
                continue;
            }
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
    // Post-auth cap: a buggy/compromised child shouldn't be able to grow
    // our buffer without bound by never sending a newline.
    static constexpr int kPostAuthCap = 1 * 1024 * 1024;
    if (m_authenticated && m_readBuf.size() > kPostAuthCap) {
        rejectClient("oversized post-auth payload");
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

