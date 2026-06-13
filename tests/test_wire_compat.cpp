// Forward-compat of the D-Bus wire: a NEWER client (whose ConnectionDto /
// InterfaceStatsDto readers expect the latest append-only fields) must
// decode an OLDER agent's SHORTER struct gracefully — never read past the
// end of the structure (which aborts the process under QtDBus, the crash a
// user hit running a new nqiftop against a v0.5 agent).
//
// We can't synthesise a truncated wire locally (a QDBusArgument can't be
// marshalled and demarshalled in one process without a bus), so this test
// registers a tiny service on the session bus that returns OLD-shaped lists
// (ConnectionDto WITHOUT the v0.6 `reason` byte; InterfaceStatsDto WITHOUT
// the v0.3 ifindex/operstate/errors tail) and demarshals the raw reply with
// the PRODUCTION operator>>.

#include <QtDBus/QtDBus>
#include <QtTest/QtTest>

#include "dbus/Types.h"
#include "backend/Connection.h"

// ---- "old" DTOs: the pre-v0.6 connection struct (22 fields, no reason) and
//      the pre-v0.3 interface struct (10 fields, no ifindex tail). ----------

namespace oldwire {

struct OldContainer { QString runtime, id, name; };

QDBusArgument &operator<<(QDBusArgument &a, const OldContainer &c)
{ a.beginStructure(); a << c.runtime << c.id << c.name; a.endStructure(); return a; }
const QDBusArgument &operator>>(const QDBusArgument &a, OldContainer &c)
{ a.beginStructure(); a >> c.runtime >> c.id >> c.name; a.endStructure(); return a; }

struct OldConn {
    quint8 proto=6, localFamily=4; QString localAddress; quint16 localPort=0;
    quint8 remoteFamily=4; QString remoteAddress; quint16 remotePort=0;
    quint64 rxBytes=0, txBytes=0, rxPackets=0, txPackets=0;
    QString iface; quint8 direction=0; quint32 ifIndex=0; quint8 tcpState=0;
    quint32 pid=0, uid=0; QString comm, containerRuntime, containerId, containerName;
    QList<OldContainer> containerChain;
    // NOTE: no `reason` — this is the v0.5 wire.
};

QDBusArgument &operator<<(QDBusArgument &a, const OldConn &c)
{
    a.beginStructure();
    a << c.proto << c.localFamily << c.localAddress << c.localPort
      << c.remoteFamily << c.remoteAddress << c.remotePort
      << c.rxBytes << c.txBytes << c.rxPackets << c.txPackets
      << c.iface << c.direction << c.ifIndex << c.tcpState
      << c.pid << c.uid << c.comm
      << c.containerRuntime << c.containerId << c.containerName
      << c.containerChain;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, OldConn &c)
{
    a.beginStructure();
    a >> c.proto >> c.localFamily >> c.localAddress >> c.localPort
      >> c.remoteFamily >> c.remoteAddress >> c.remotePort
      >> c.rxBytes >> c.txBytes >> c.rxPackets >> c.txPackets
      >> c.iface >> c.direction >> c.ifIndex >> c.tcpState
      >> c.pid >> c.uid >> c.comm
      >> c.containerRuntime >> c.containerId >> c.containerName
      >> c.containerChain;
    a.endStructure();
    return a;
}
using OldConnList = QList<OldConn>;

struct OldIface {
    QString name, type; quint32 mtu=0; QStringList addresses;
    quint64 rxBytes=0, txBytes=0, rxPackets=0, txPackets=0;
    bool isUp=false, isLoopback=false;
    // NOTE: no ifindex/operstate/errors tail — this is the pre-v0.3 wire.
};
QDBusArgument &operator<<(QDBusArgument &a, const OldIface &s)
{
    a.beginStructure();
    a << s.name << s.type << s.mtu << s.addresses
      << s.rxBytes << s.txBytes << s.rxPackets << s.txPackets
      << s.isUp << s.isLoopback;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, OldIface &s)
{
    a.beginStructure();
    a >> s.name >> s.type >> s.mtu >> s.addresses
      >> s.rxBytes >> s.txBytes >> s.rxPackets >> s.txPackets
      >> s.isUp >> s.isLoopback;
    a.endStructure();
    return a;
}
using OldIfaceList = QList<OldIface>;

} // namespace oldwire

Q_DECLARE_METATYPE(oldwire::OldContainer)
Q_DECLARE_METATYPE(oldwire::OldConn)
Q_DECLARE_METATYPE(oldwire::OldIface)

// A minimal "old agent" object exporting GetConnections/GetInterfaces that
// return the SHORTER structs.
class OldAgent : public QObject {
    Q_OBJECT
public:
    explicit OldAgent(QObject *p = nullptr) : QObject(p) {}
public slots:
    oldwire::OldConnList GetConnections()
    {
        oldwire::OldConn c;
        c.proto = 17; c.localFamily = 4; c.localAddress = QStringLiteral("10.42.0.122");
        c.localPort = 9994; c.remoteFamily = 4; c.remoteAddress = QStringLiteral("84.17.53.155");
        c.remotePort = 9993; c.rxBytes = 4242; c.txBytes = 1717;
        c.iface = QStringLiteral("eno1"); c.direction = 0; c.ifIndex = 3; c.tcpState = 0;
        c.pid = 0; // unattributed (forwarded)
        return { c };
    }
    oldwire::OldIfaceList GetInterfaces()
    {
        oldwire::OldIface s;
        s.name = QStringLiteral("eno1"); s.type = QStringLiteral("ethernet");
        s.mtu = 1500; s.addresses = { QStringLiteral("10.0.0.61/24") };
        s.rxBytes = 1ULL << 33; s.txBytes = 1ULL << 32; s.isUp = true;
        return { s };
    }
};

class TestWireCompat : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qiftop::dbus::registerTypes();
        qRegisterMetaType<oldwire::OldContainer>();
        qRegisterMetaType<oldwire::OldConn>();
        qRegisterMetaType<oldwire::OldIface>();
        qDBusRegisterMetaType<oldwire::OldContainer>();
        qDBusRegisterMetaType<oldwire::OldConn>();
        qDBusRegisterMetaType<oldwire::OldConnList>();
        qDBusRegisterMetaType<oldwire::OldIface>();
        qDBusRegisterMetaType<oldwire::OldIfaceList>();

        m_bus = QDBusConnection::sessionBus();
        if (!m_bus.isConnected())
            QSKIP("no session bus (run under dbus-run-session)");
        m_agent = new OldAgent(this);
        QVERIFY(m_bus.registerObject(QStringLiteral("/old"), m_agent,
                                     QDBusConnection::ExportAllSlots));
    }

    // The crux: call the old service, take the RAW reply, and demarshal it
    // with the PRODUCTION (newer, longer) reader. Must not abort, and must
    // recover the base fields with the missing trailing field left default.
    void newReaderDecodesOldConnectionStruct()
    {
        if (!m_bus.isConnected()) QSKIP("no session bus");
        auto call = QDBusMessage::createMethodCall(
            m_bus.baseService(), QStringLiteral("/old"),
            QString(), QStringLiteral("GetConnections"));
        const QDBusMessage reply = m_bus.call(call);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

        const auto args = reply.arguments();
        QVERIFY(!args.isEmpty());
        QDBusArgument arg = args.at(0).value<QDBusArgument>();

        // Production reader expecting the v0.6 (23-field) struct.
        qiftop::dbus::ConnectionDtoList list;
        arg >> list;                          // <-- would abort pre-fix

        QCOMPARE(list.size(), 1);
        const auto &d = list.at(0);
        QCOMPARE(d.proto, quint8(17));
        QCOMPARE(d.localAddress, QStringLiteral("10.42.0.122"));
        QCOMPARE(d.localPort, quint16(9994));
        QCOMPARE(d.remotePort, quint16(9993));
        QCOMPARE(d.rxBytes, quint64(4242));
        QCOMPARE(d.pid, quint32(0));
        // The field the old agent never sent stays at its default.
        QCOMPARE(d.reason, quint8(AttributionReason::Resolved));

        // fromDto must also survive and clamp sanely.
        const Connection c = qiftop::dbus::fromDto(d);
        QCOMPARE(c.proto, L4Proto::Udp);
        QCOMPARE(c.reason, AttributionReason::Resolved);
    }

    void newReaderDecodesOldInterfaceStruct()
    {
        if (!m_bus.isConnected()) QSKIP("no session bus");
        auto call = QDBusMessage::createMethodCall(
            m_bus.baseService(), QStringLiteral("/old"),
            QString(), QStringLiteral("GetInterfaces"));
        const QDBusMessage reply = m_bus.call(call);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);

        const auto args = reply.arguments();
        QVERIFY(!args.isEmpty());
        QDBusArgument arg = args.at(0).value<QDBusArgument>();

        qiftop::dbus::InterfaceStatsDtoList list;
        arg >> list;                          // <-- would abort pre-fix

        QCOMPARE(list.size(), 1);
        const auto &d = list.at(0);
        QCOMPARE(d.name, QStringLiteral("eno1"));
        QCOMPARE(d.mtu, quint32(1500));
        QVERIFY(d.isUp);
        // Trailing v0.3 fields the old agent never sent stay default.
        QCOMPARE(d.ifIndex, quint32(0));
        QCOMPARE(d.operState, quint8(0));
        QCOMPARE(d.rxErrors, quint64(0));
    }

private:
    QDBusConnection m_bus = QDBusConnection::sessionBus();
    OldAgent       *m_agent = nullptr;
};

QTEST_MAIN(TestWireCompat)
#include "test_wire_compat.moc"
