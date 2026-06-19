#include "DBusConnectionMonitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

#include <algorithm>
#include <limits>

#include "backend/ProcessDetails.h"
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

DBusConnectionMonitor::~DBusConnectionMonitor() { DBusConnectionMonitor::stop(); }

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
    conn.connect(QString::fromLatin1(kService),
                 QString::fromLatin1(kPath),
                 QString::fromLatin1(kIface),
                 QStringLiteral("AttributionEagernessChanged"),
                 this, SLOT(onAttributionEagernessChanged(QString)));
    conn.connect(QString::fromLatin1(kService),
                 QString::fromLatin1(kPath),
                 QString::fromLatin1(kIface),
                 QStringLiteral("AttributionChanged"),
                 this, SLOT(onAttributionChanged(QDBusMessage)));
    requestInitialSnapshot();
    if (m_desiredMs > 0)
        sendDesiredIntervalAsync(m_desiredMs);
    if (!m_desiredEagerness.isEmpty())
        setDesiredAttributionEagerness(m_desiredEagerness);
}

void DBusConnectionMonitor::setDesiredIntervalMs(int ms)
{
    ms = std::max(ms, 0);
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

void DBusConnectionMonitor::requestProcessDetails(qint32 pid)
{
    if (pid <= 0) return;
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("GetProcessDetails"));
    call << QVariant::fromValue<quint32>(static_cast<quint32>(pid));
    auto *watcher = new QDBusPendingCallWatcher(conn.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<qiftop::dbus::ProcessDetailsDto> reply = *w;
                if (reply.isError()) {
                    qCInfo(lcVerbose) << "GetProcessDetails failed:"
                                      << reply.error().message();
                    return;
                }
                const auto d = reply.value();
                backend::ProcessDetails out;
                out.pid              = (d.pid <= quint32(std::numeric_limits<qint32>::max()))
                                           ? qint32(d.pid) : 0;
                out.uid              = d.uid;
                out.comm             = d.comm;
                out.exe              = d.exe;
                out.cmdline          = d.cmdline;
                out.cwd              = d.cwd;
                out.startTimeJiffies = d.startTimeJiffies;
                emit processDetailsReady(out);
            });
}

void DBusConnectionMonitor::setDesiredAttributionEagerness(const QString &mode)
{
    // Remember the request so a restart re-asserts it (mirrors m_desiredMs).
    m_desiredEagerness = mode;
    if (!m_started) return;
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("SetDesiredAttributionEagerness"));
    call << mode;
    // Fire-and-forget: the resulting effective mode arrives via the
    // AttributionEagernessChanged signal (or the property). We don't block
    // on the string return.
    conn.asyncCall(call);
}

void DBusConnectionMonitor::onAttributionEagernessChanged(const QString &mode)
{
    emit attributionEagernessChanged(mode);
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
    conn.disconnect(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface),
                    QStringLiteral("AttributionEagernessChanged"),
                    this, SLOT(onAttributionEagernessChanged(QString)));
    conn.disconnect(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface),
                    QStringLiteral("AttributionChanged"),
                    this, SLOT(onAttributionChanged(QDBusMessage)));
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
                const QDBusMessage reply = w->reply();
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    qCWarning(lcVerbose).noquote()
                        << "DBusConnectionMonitor: GetConnections failed:"
                        << reply.errorMessage();
                    return;
                }
                // Demarshal the RAW reply rather than QDBusPendingReply<T>:
                // an OLDER agent's shorter struct has a D-Bus signature that
                // won't match our registered ConnectionDtoList type, so Qt's
                // typed reply would reject it outright ("unexpected reply
                // signature"). Reading the raw QDBusArgument routes through the
                // append-only-tolerant operator>> instead (mirrors the
                // ConnectionsChanged signal path). Future-proofs new client +
                // old agent without an interface bump.
                const auto args = reply.arguments();
                if (args.isEmpty()) return;
                QDBusArgument arg = args.at(0).value<QDBusArgument>();
                qiftop::dbus::ConnectionDtoList list;
                arg >> list;
                emit connectionsUpdated(qiftop::dbus::fromDtos(list));
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

void DBusConnectionMonitor::onAttributionChanged(const QDBusMessage &msg)
{
    // Same (t monotonicMs, a(...) conns) layout as ConnectionsChanged, but the
    // rows are an attribution-only patch — emit on the refinement signal so the
    // aggregator updates attribution columns without touching the rate series.
    const auto args = msg.arguments();
    QDBusArgument arg;
    if (args.size() >= 2)
        arg = args.at(1).value<QDBusArgument>();
    else if (args.size() == 1)
        arg = args.at(0).value<QDBusArgument>();
    else
        return;
    qiftop::dbus::ConnectionDtoList list;
    arg >> list;
    if (!list.isEmpty())
        emit connectionsAttributionRefined(qiftop::dbus::fromDtos(list));
}

void DBusConnectionMonitor::onPermissionDenied(const QString &detail)
{
    emit permissionDenied(detail);
}

} // namespace qiftop::backend::dbus_client
