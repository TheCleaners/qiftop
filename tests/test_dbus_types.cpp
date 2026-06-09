// Round-trip tests for the qiftop::dbus::ConnectionDto wire format.
// Locks in the contract added for v0.2 of the agent (IANA proto numbers,
// direction-on-wire). Bump these tests + the contract docs together if
// you ever bump to NetworkAgent2.

#include <QObject>
#include <QtTest/QtTest>

#include <QDBusArgument>

#include "dbus/Types.h"
#include "backend/Connection.h"

using qiftop::backend::ContainerInfo;
using qiftop::backend::ProcessInfo;

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
        original.ifIndex        = 42;
        original.tcpState       = TcpState::Established;

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
        QCOMPARE(dtoOut.ifIndex,       quint32(42));
        QCOMPARE(dtoOut.tcpState,      quint8(TcpState::Established));

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
        QCOMPARE(rebuilt.ifIndex,        quint32(42));
        QCOMPARE(rebuilt.tcpState,       TcpState::Established);
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

    void fromDtoClampsOutOfRangeTcpState()
    {
        // Same safety net for TCP state — a future kernel could grow the
        // enum (it already did once: SYN_SENT2 came in 4.x) and we don't
        // want UB on the receiver.
        qiftop::dbus::ConnectionDto d;
        d.proto    = 6;
        d.tcpState = 250;
        const auto c = qiftop::dbus::fromDto(d);
        QCOMPARE(c.tcpState, TcpState::None);
    }

    void interfaceStatsDtoRoundTrip()
    {
        InterfaceStats s;
        s.name       = QStringLiteral("eth0");
        s.type       = QStringLiteral("ethernet");
        s.mtu        = 1500;
        s.addresses  = {QStringLiteral("10.0.0.5/24")};
        s.rxBytes    = 1ULL << 40;          // exercises >32-bit values
        s.txBytes    = 2ULL << 40;
        s.rxPackets  = 1'000'000;
        s.txPackets  = 500'000;
        s.isUp       = true;
        s.isLoopback = false;
        s.ifIndex    = 7;
        s.operState  = 6;                    // IF_OPER_UP
        s.rxErrors   = 11;
        s.txErrors   = 22;
        s.rxDropped  = 33;
        s.txDropped  = 44;

        const auto dto = qiftop::dbus::toDto(s);
        QVariant v;
        v.setValue(dto);
        const auto round = qvariant_cast<qiftop::dbus::InterfaceStatsDto>(v);
        const auto back  = qiftop::dbus::fromDto(round);

        QCOMPARE(back.name, s.name);
        QCOMPARE(back.mtu,  s.mtu);
        QCOMPARE(back.rxBytes,   s.rxBytes);
        QCOMPARE(back.txBytes,   s.txBytes);
        QCOMPARE(back.ifIndex,   s.ifIndex);
        QCOMPARE(back.operState, s.operState);
        QCOMPARE(back.rxErrors,  s.rxErrors);
        QCOMPARE(back.txErrors,  s.txErrors);
        QCOMPARE(back.rxDropped, s.rxDropped);
        QCOMPARE(back.txDropped, s.txDropped);
    }

    // ---- v0.4 attribution wire fields ----------------------------------

    void connectionDtoCarriesAttribution()
    {
        // Bulk attribution: pid/uid/comm/container.* always present (zero/
        // empty when no resolver is wired). EXE / cmdline are NOT on the
        // wire — fetched on demand via GetProcessDetails per design
        // principle "default-cheap pipeline".
        Connection c;
        c.proto      = L4Proto::Tcp;
        c.local.address  = QHostAddress(QStringLiteral("10.0.0.5"));
        c.local.port = 8080;
        c.remote.address = QHostAddress(QStringLiteral("203.0.113.7"));
        c.remote.port    = 443;
        c.process.pid = 1234;
        c.process.uid = 33;
        c.process.comm = QStringLiteral("nginx");
        c.container = ContainerInfo{
            QStringLiteral("docker"),
            QStringLiteral("af85275074f5"),
            QStringLiteral("web-frontend")};
        c.containerChain = {
            ContainerInfo{QStringLiteral("kubernetes"),
                          QStringLiteral("1eb53288-f9d"), {}},
            ContainerInfo{QStringLiteral("docker"),
                          QStringLiteral("af85275074f5"),
                          QStringLiteral("web-frontend")},
        };

        const auto dto = qiftop::dbus::toDto(c);
        QCOMPARE(dto.pid, quint32(1234));
        QCOMPARE(dto.uid, quint32(33));
        QCOMPARE(dto.comm, QStringLiteral("nginx"));
        QCOMPARE(dto.containerRuntime, QStringLiteral("docker"));
        QCOMPARE(dto.containerId,      QStringLiteral("af85275074f5"));
        QCOMPARE(dto.containerName,    QStringLiteral("web-frontend"));
        QCOMPARE(dto.containerChain.size(), 2);
        QCOMPARE(dto.containerChain[0].runtime,
                 QStringLiteral("kubernetes"));
        QCOMPARE(dto.containerChain[1].id,
                 QStringLiteral("af85275074f5"));

        // Round-trip through QVariant must preserve every field.
        QVariant v;
        v.setValue(dto);
        const auto round = qvariant_cast<qiftop::dbus::ConnectionDto>(v);
        QCOMPARE(round.pid,            dto.pid);
        QCOMPARE(round.uid,            dto.uid);
        QCOMPARE(round.comm,           dto.comm);
        QCOMPARE(round.containerRuntime, dto.containerRuntime);
        QCOMPARE(round.containerId,      dto.containerId);
        QCOMPARE(round.containerName,    dto.containerName);
        QCOMPARE(round.containerChain.size(), dto.containerChain.size());
        for (int i = 0; i < dto.containerChain.size(); ++i) {
            QCOMPARE(round.containerChain[i].runtime,
                     dto.containerChain[i].runtime);
            QCOMPARE(round.containerChain[i].id,
                     dto.containerChain[i].id);
            QCOMPARE(round.containerChain[i].name,
                     dto.containerChain[i].name);
        }

        const auto rebuilt = qiftop::dbus::fromDto(round);
        QCOMPARE(rebuilt.process.pid,  qint32(1234));
        QCOMPARE(rebuilt.process.comm, QStringLiteral("nginx"));
        QCOMPARE(rebuilt.container.runtime, QStringLiteral("docker"));
        QCOMPARE(rebuilt.containerChain.size(), 2);
    }

    void connectionDtoOmitsAttributionWhenAbsent()
    {
        // When the agent has no resolver wired, attribution fields are
        // their zero-init defaults. Round-trip must keep them zero/empty
        // rather than e.g. inventing pid=1 by accident.
        Connection c;
        c.proto = L4Proto::Udp;
        const auto dto = qiftop::dbus::toDto(c);
        QCOMPARE(dto.pid, quint32(0));
        QCOMPARE(dto.uid, quint32(0));
        QVERIFY(dto.comm.isEmpty());
        QVERIFY(dto.containerRuntime.isEmpty());
        QVERIFY(dto.containerId.isEmpty());
        QVERIFY(dto.containerName.isEmpty());
        QVERIFY(dto.containerChain.isEmpty());

        QVariant v;
        v.setValue(dto);
        const auto round = qvariant_cast<qiftop::dbus::ConnectionDto>(v);
        QCOMPARE(round.pid, quint32(0));
        QVERIFY(round.comm.isEmpty());
        QVERIFY(round.containerChain.isEmpty());
    }
};

QTEST_MAIN(TestDbusTypes)
#include "test_dbus_types.moc"
