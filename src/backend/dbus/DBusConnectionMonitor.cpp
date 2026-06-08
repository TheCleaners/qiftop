#include "DBusConnectionMonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#include "dbus/Types.h"
#include "util/Logging.h"

namespace qiftop::backend::dbus_client {

namespace {
constexpr auto kService = "org.qiftop.NetworkAgent1";
constexpr auto kPath    = "/org/qiftop/NetworkAgent1/Connections";
constexpr auto kIface   = "org.qiftop.NetworkAgent1.Connections";
} // namespace

static QDBusConnection bus(bool session)
{
    return session ? QDBusConnection::sessionBus() : QDBusConnection::systemBus();
}

DBusConnectionMonitor::DBusConnectionMonitor(bool useSessionBus, QObject *parent)
    : ConnectionMonitor(parent)
    , m_useSessionBus(useSessionBus)
{
}

DBusConnectionMonitor::~DBusConnectionMonitor() { stop(); }

void DBusConnectionMonitor::start()
{
    if (m_started) return;
    m_started = true;

    auto conn = bus(m_useSessionBus);
    conn.connect(QString::fromLatin1(kService),
                 QString::fromLatin1(kPath),
                 QString::fromLatin1(kIface),
                 QStringLiteral("ConnectionsChanged"),
                 this, SLOT(onConnectionsChanged(QDBusMessage)));
    conn.connect(QString::fromLatin1(kService),
                 QString::fromLatin1(kPath),
                 QString::fromLatin1(kIface),
                 QStringLiteral("PermissionDenied"),
                 this, SLOT(onPermissionDenied(QString)));
    requestInitialSnapshot();
    if (m_desiredMs > 0)
        sendDesiredIntervalAsync(m_desiredMs);
}

void DBusConnectionMonitor::setDesiredIntervalMs(int ms)
{
    if (ms < 0) ms = 0;
    m_desiredMs = ms;
    if (m_started)
        sendDesiredIntervalAsync(ms);
}

void DBusConnectionMonitor::sendDesiredIntervalAsync(int ms)
{
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("SetDesiredIntervalMs"));
    call << QVariant::fromValue<quint32>(static_cast<quint32>(ms));
    conn.asyncCall(call);
}

void DBusConnectionMonitor::stop()
{
    if (!m_started) return;
    m_started = false;
    auto conn = bus(m_useSessionBus);
    conn.disconnect(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface), QStringLiteral("ConnectionsChanged"),
                    this, SLOT(onConnectionsChanged(QDBusMessage)));
    conn.disconnect(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface), QStringLiteral("PermissionDenied"),
                    this, SLOT(onPermissionDenied(QString)));
}

void DBusConnectionMonitor::requestInitialSnapshot()
{
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("GetConnections"));
    auto *watcher = new QDBusPendingCallWatcher(conn.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<qiftop::dbus::ConnectionDtoList> reply = *w;
                if (reply.isError()) {
                    qCWarning(lcVerbose).noquote()
                        << "DBusConnectionMonitor: GetConnections failed:"
                        << reply.error().message();
                    return;
                }
                emit connectionsUpdated(qiftop::dbus::fromDtos(reply.value()));
            });
}

void DBusConnectionMonitor::onConnectionsChanged(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    // v0.3 wire layout: (t monotonicMs, a(...) conns). Pre-0.3 agents only
    // emit the array; index defensively so a pre-0.3 agent doesn't crash a
    // new client (the version probe is what gates the bus name anyway).
    QDBusArgument arg;
    if (args.size() >= 2)
        arg = args.at(1).value<QDBusArgument>();
    else if (args.size() == 1)
        arg = args.at(0).value<QDBusArgument>();
    else
        return;
    qiftop::dbus::ConnectionDtoList list;
    arg >> list;
    emit connectionsUpdated(qiftop::dbus::fromDtos(list));
}

void DBusConnectionMonitor::onPermissionDenied(const QString &detail)
{
    emit permissionDenied(detail);
}

} // namespace qiftop::backend::dbus_client
