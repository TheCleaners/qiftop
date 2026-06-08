// Round-trip tests for the qiftop::dbus::ConnectionDto wire format.
// Locks in the contract added for v0.2 of the agent (IANA proto numbers,
// direction-on-wire). Bump these tests + the contract docs together if
// you ever bump to NetworkAgent2.

#include <QObject>
#include <QtTest/QtTest>

#include <QDBusArgument>

#include "dbus/Types.h"
#include "backend/Connection.h"

class TestDbusTypes : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qiftop::dbus::registerTypes();
    }

    void protoMapsToIanaNumbers()
    {
        QCOMPARE(toIanaProto(L4Proto::Tcp),     quint8(6));
        QCOMPARE(toIanaProto(L4Proto::Udp),     quint8(17));
        QCOMPARE(toIanaProto(L4Proto::Icmp),    quint8(1));
        QCOMPARE(toIanaProto(L4Proto::IcmpV6),  quint8(58));
        QCOMPARE(toIanaProto(L4Proto::Unknown), quint8(0));

        QCOMPARE(fromIanaProto(6),   L4Proto::Tcp);
        QCOMPARE(fromIanaProto(17),  L4Proto::Udp);
        QCOMPARE(fromIanaProto(1),   L4Proto::Icmp);
        QCOMPARE(fromIanaProto(58),  L4Proto::IcmpV6);
        QCOMPARE(fromIanaProto(0),   L4Proto::Unknown);
        QCOMPARE(fromIanaProto(99),  L4Proto::Unknown);   // unknown -> Unknown
        QCOMPARE(fromIanaProto(255), L4Proto::Unknown);
    }

    void toDtoUsesIanaProto()
    {
        Connection c;
        c.proto = L4Proto::Tcp;
        const auto d = qiftop::dbus::toDto(c);
        QCOMPARE(d.proto, quint8(6));     // NOT 1 (would be L4Proto::Tcp index)
    }

    void connectionDtoRoundTripsThroughQDBusArgument()
    {
        Connection original;
        original.proto          = L4Proto::Tcp;
        original.local.address  = QHostAddress(QStringLiteral("192.168.1.5"));
        original.local.port     = 51234;
        original.remote.address = QHostAddress(QStringLiteral("142.250.80.46"));
        original.remote.port    = 443;
        original.rxBytes        = 1234567;
        original.txBytes        = 89012;
        original.rxPackets      = 1001;
        original.txPackets      = 502;
        original.iface          = QStringLiteral("wlp1s0");
        original.direction      = Direction::Outbound;

        // DTO -> QVariant -> DTO via the marshalling operators
        const auto dtoIn = qiftop::dbus::toDto(original);
        QVERIFY(QMetaType::fromType<qiftop::dbus::ConnectionDto>().isRegistered());

        QVariant v;
        v.setValue(dtoIn);
        QVERIFY(v.canConvert<qiftop::dbus::ConnectionDto>());
        const auto dtoOut = qvariant_cast<qiftop::dbus::ConnectionDto>(v);

        QCOMPARE(dtoOut.proto,         dtoIn.proto);
        QCOMPARE(dtoOut.proto,         quint8(6));         // IANA TCP
        QCOMPARE(dtoOut.localAddress,  dtoIn.localAddress);
        QCOMPARE(dtoOut.localPort,     dtoIn.localPort);
        QCOMPARE(dtoOut.remoteAddress, dtoIn.remoteAddress);
        QCOMPARE(dtoOut.remotePort,    dtoIn.remotePort);
        QCOMPARE(dtoOut.rxBytes,       dtoIn.rxBytes);
        QCOMPARE(dtoOut.txBytes,       dtoIn.txBytes);
        QCOMPARE(dtoOut.iface,         dtoIn.iface);
        QCOMPARE(dtoOut.direction,     quint8(Direction::Outbound));

        // Full round-trip back through fromDto must restore the value semantics
        const Connection rebuilt = qiftop::dbus::fromDto(dtoOut);
        QCOMPARE(rebuilt.proto,          L4Proto::Tcp);
        QCOMPARE(rebuilt.local.address,  original.local.address);
        QCOMPARE(rebuilt.local.port,     original.local.port);
        QCOMPARE(rebuilt.remote.address, original.remote.address);
        QCOMPARE(rebuilt.remote.port,    original.remote.port);
        QCOMPARE(rebuilt.rxBytes,        original.rxBytes);
        QCOMPARE(rebuilt.iface,          original.iface);
        QCOMPARE(rebuilt.direction,      Direction::Outbound);
    }

    void fromDtoClampsOutOfRangeDirection()
    {
        // A malicious or future-extended agent could ship direction=99;
        // fromDto must NOT static_cast it blindly (which would be UB on
        // the receiver) but clamp to Unknown.
        qiftop::dbus::ConnectionDto d;
        d.proto     = 6;
        d.direction = 99;
        const auto c = qiftop::dbus::fromDto(d);
        QCOMPARE(c.direction, Direction::Unknown);
    }
};

QTEST_MAIN(TestDbusTypes)
#include "test_dbus_types.moc"
