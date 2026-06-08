#include "DBusNetworkMonitor.h"

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
constexpr auto kPath    = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kIface   = "org.qiftop.NetworkAgent1.Interfaces";
} // namespace

DBusNetworkMonitor::DBusNetworkMonitor(bool useSessionBus, QObject *parent)
    : NetworkMonitor(parent)
    , m_useSessionBus(useSessionBus)
{
}

DBusNetworkMonitor::~DBusNetworkMonitor() { stop(); }

static QDBusConnection bus(bool session)
{
    return session ? QDBusConnection::sessionBus() : QDBusConnection::systemBus();
}

void DBusNetworkMonitor::start()
{
    if (m_started) return;
    m_started = true;

    auto conn = bus(m_useSessionBus);
    const bool ok = conn.connect(QString::fromLatin1(kService),
                                 QString::fromLatin1(kPath),
                                 QString::fromLatin1(kIface),
                                 QStringLiteral("StatsChanged"),
                                 this,
                                 SLOT(onStatsChanged(QDBusMessage)));
    if (!ok) {
        qCWarning(lcVerbose) << "DBusNetworkMonitor: failed to subscribe to StatsChanged";
    }
    // Best-effort: pre-Capabilities agents don't emit CadenceChanged, but
    // subscribing is harmless — we just never get any signals.
    const bool okCad = conn.connect(QString::fromLatin1(kService),
                                    QString::fromLatin1(kPath),
                                    QString::fromLatin1(kIface),
                                    QStringLiteral("CadenceChanged"),
                                    this,
                                    SLOT(onAgentCadenceChanged(QDBusMessage)));
    if (!okCad) {
        qCWarning(lcVerbose) << "DBusNetworkMonitor: failed to subscribe to CadenceChanged";
    }
    requestInitialSnapshot();
    if (m_desiredMs > 0)
        sendDesiredIntervalAsync(m_desiredMs);
}

void DBusNetworkMonitor::stop()
{
    if (!m_started) return;
    m_started = false;
    auto conn = bus(m_useSessionBus);
    conn.disconnect(QString::fromLatin1(kService),
                    QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface),
                    QStringLiteral("StatsChanged"),
                    this, SLOT(onStatsChanged(QDBusMessage)));
    conn.disconnect(QString::fromLatin1(kService),
                    QString::fromLatin1(kPath),
                    QString::fromLatin1(kIface),
                    QStringLiteral("CadenceChanged"),
                    this, SLOT(onAgentCadenceChanged(QDBusMessage)));
}

void DBusNetworkMonitor::requestInitialSnapshot()
{
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("GetInterfaces"));
    auto pending = conn.asyncCall(call);
    auto *watcher = new QDBusPendingCallWatcher(pending, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                QDBusPendingReply<qiftop::dbus::InterfaceStatsDtoList> reply = *w;
                if (reply.isError()) {
                    qCWarning(lcVerbose).noquote()
                        << "DBusNetworkMonitor: GetInterfaces failed:"
                        << reply.error().message();
                    return;
                }
                emit statsUpdated(qiftop::dbus::fromDtos(reply.value()));
            });
}

void DBusNetworkMonitor::onStatsChanged(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    // v0.3 wire layout: (t monotonicMs, a(...) stats). Older agents only
    // emit the array; tolerate both during alpha rollover.
    QDBusArgument arg;
    if (args.size() >= 2)
        arg = args.at(1).value<QDBusArgument>();
    else if (args.size() == 1)
        arg = args.at(0).value<QDBusArgument>();
    else
        return;
    qiftop::dbus::InterfaceStatsDtoList list;
    arg >> list;
    emit statsUpdated(qiftop::dbus::fromDtos(list));
}

void DBusNetworkMonitor::onAgentCadenceChanged(const QDBusMessage &msg)
{
    const auto args = msg.arguments();
    if (args.isEmpty()) return;
    // The DBus signature is 'u'; Qt unmarshals to quint32 in a QVariant.
    bool ok = false;
    const int ms = static_cast<int>(args.first().toUInt(&ok));
    if (!ok) return;
    emit agentCadenceChanged(ms);
}

void DBusNetworkMonitor::setDesiredIntervalMs(int ms)
{
    if (ms < 0) ms = 0;
    m_desiredMs = ms;
    if (m_started)
        sendDesiredIntervalAsync(ms);
}

void DBusNetworkMonitor::sendDesiredIntervalAsync(int ms)
{
    auto conn = bus(m_useSessionBus);
    auto call = QDBusMessage::createMethodCall(QString::fromLatin1(kService),
                                               QString::fromLatin1(kPath),
                                               QString::fromLatin1(kIface),
                                               QStringLiteral("SetDesiredIntervalMs"));
    call << QVariant::fromValue<quint32>(static_cast<quint32>(ms));
    // Fire-and-forget; we don't care about the reply (the agent has no
    // return value here). Using asyncCall + noReplyExpected would also
    // work but Qt's API is simpler this way.
    conn.asyncCall(call);
}

} // namespace qiftop::backend::dbus_client
