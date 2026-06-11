#include <QSignalSpy>
#include <QTest>

#include <optional>

#include "agent/ConnectionsService.h"
#include "agent/InterfacesService.h"
#include "backend/PlatformInfo.h"
#include "backend/ProcessResolver.h"
#include "dbus/Types.h"
#include "fakes/FakeMonitors.h"

using qiftop::backend::ContainerInfo;
using qiftop::backend::ProcessInfo;
using qiftop::backend::ProcessResolver;

namespace {

class FakeResolver final : public ProcessResolver {
public:
    bool initialize() override { return true; }
    QStringList capabilities() const override { return {}; }

    qint32 resolvePid(const Connection &flow) override
    {
        inspectedPorts << flow.local.port;
        return m_pidByLocalPort.value(flow.local.port, 0);
    }

    std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        ++enrichCalls[pid];
        if (auto it = m_processByPid.constFind(pid); it != m_processByPid.constEnd())
            return it.value();
        return std::nullopt;
    }

    std::optional<ContainerInfo> resolveContainerForPid(qint32 pid) override
    {
        ++containerCalls[pid];
        if (auto it = m_containerByPid.constFind(pid); it != m_containerByPid.constEnd())
            return it.value();
        return std::nullopt;
    }

    QList<ContainerInfo> resolveContainerChainForPid(qint32 pid) override
    {
        ++chainCalls[pid];
        return m_chainByPid.value(pid);
    }

    void mapPortToProcess(quint16 port, ProcessInfo process)
    {
        m_pidByLocalPort.insert(port, process.pid);
        m_processByPid.insert(process.pid, std::move(process));
    }

    void mapContainer(qint32 pid, ContainerInfo container)
    {
        m_containerByPid.insert(pid, std::move(container));
    }

    void mapChain(qint32 pid, QList<ContainerInfo> chain)
    {
        m_chainByPid.insert(pid, std::move(chain));
    }

    QList<quint16> inspectedPorts;
    QHash<qint32, int> enrichCalls;
    QHash<qint32, int> containerCalls;
    QHash<qint32, int> chainCalls;

private:
    QHash<quint16, qint32> m_pidByLocalPort;
    QHash<qint32, ProcessInfo> m_processByPid;
    QHash<qint32, ContainerInfo> m_containerByPid;
    QHash<qint32, QList<ContainerInfo>> m_chainByPid;
};

Connection makeFlow(quint16 localPort, quint64 bytes)
{
    Connection c;
    c.proto = L4Proto::Tcp;
    c.iface = QStringLiteral("eth0");
    c.ifIndex = 7;
    c.local.address = QHostAddress(QStringLiteral("10.0.0.10"));
    c.local.port = localPort;
    c.remote.address = QHostAddress(QStringLiteral("198.51.100.20"));
    c.remote.port = 443;
    c.rxBytes = bytes;
    return c;
}

} // namespace

class TestServices : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        qRegisterMetaType<InterfaceStats>();
        qRegisterMetaType<QList<InterfaceStats>>();
        qRegisterMetaType<Connection>();
        qRegisterMetaType<QList<Connection>>();
        qRegisterMetaType<qiftop::dbus::InterfaceStatsDtoList>();
        qRegisterMetaType<qiftop::dbus::ConnectionDtoList>();
        qiftop::dbus::registerTypes();
    }

    void interfacesServiceCachesAndSignalsFakeSnapshot()
    {
        qiftop::tests::FakeNetworkMonitor monitor;
        qiftop::agent::InterfacesService service(&monitor);
        QSignalSpy changed(&service, &qiftop::agent::InterfacesService::StatsChanged);
        QVERIFY(changed.isValid());

        auto eth0 = qiftop::tests::mkIface("eth0", 2, 100, 200);
        eth0.addresses = {QStringLiteral("192.0.2.10/24")};
        eth0.mtu = 1500;
        eth0.rxPackets = 3;
        eth0.txPackets = 4;
        eth0.rxErrors = 5;
        eth0.txDropped = 6;

        auto lo = qiftop::tests::mkIface("lo", 1, 7, 8);
        lo.type = QStringLiteral("loopback");
        lo.isLoopback = true;

        monitor.emitSnapshot({eth0, lo});

        QCOMPARE(changed.size(), 1);
        const auto signalled = changed.takeFirst().at(1)
            .value<qiftop::dbus::InterfaceStatsDtoList>();
        QCOMPARE(signalled.size(), 2);
        QCOMPARE(signalled[0].name, QStringLiteral("eth0"));
        QCOMPARE(signalled[0].ifIndex, quint32(2));
        QCOMPARE(signalled[0].addresses, QStringList{QStringLiteral("192.0.2.10/24")});
        QCOMPARE(signalled[0].rxErrors, quint64(5));
        QCOMPARE(signalled[0].txDropped, quint64(6));
        QVERIFY(signalled[1].isLoopback);

        const auto cached = service.GetInterfaces();
        QCOMPARE(cached.size(), 2);
        QCOMPARE(cached[0].name, QStringLiteral("eth0"));
        QCOMPARE(cached[1].name, QStringLiteral("lo"));
    }

    void connectionsServiceCapsAndEnrichesFakeSnapshot()
    {
        qiftop::tests::FakeConnectionMonitor monitor;
        qiftop::agent::ConnectionsService service(&monitor);
        QSignalSpy changed(&service, &qiftop::agent::ConnectionsService::ConnectionsChanged);
        QVERIFY(changed.isValid());

        const auto [ephLow, ephHigh] = qiftop::platform::ephemeralPortRange();
        Q_UNUSED(ephHigh);

        QList<quint16> generatedPorts;
        generatedPorts.reserve(4098);
        for (int candidate = 10000; generatedPorts.size() < 4098; ++candidate) {
            const auto port = static_cast<quint16>(candidate);
            if (port == ephLow)
                continue;
            generatedPorts << port;
        }
        const quint16 attributedPort = generatedPorts.back();

        FakeResolver resolver;
        ProcessInfo proc;
        proc.pid = 4242;
        proc.uid = 1000;
        proc.comm = QStringLiteral("curl");
        resolver.mapPortToProcess(attributedPort, proc);
        resolver.mapContainer(proc.pid, ContainerInfo{QStringLiteral("docker"),
                                                      QStringLiteral("abcdef123456"),
                                                      QStringLiteral("web")});
        resolver.mapChain(proc.pid,
                          {ContainerInfo{QStringLiteral("systemd"), QStringLiteral("unit:qiftop.service"), {}},
                           ContainerInfo{QStringLiteral("docker"), QStringLiteral("abcdef123456"), QStringLiteral("web")}});
        service.setProcessResolver(&resolver, true);

        QList<Connection> flows;
        flows.reserve(4098);
        for (int i = 0; i < generatedPorts.size(); ++i)
            flows << makeFlow(generatedPorts[i], static_cast<quint64>(i));

        auto directional = makeFlow(ephLow, 10'000);
        directional.remote.port = 80;
        flows << directional;

        monitor.emitSnapshot(flows);

        QCOMPARE(changed.size(), 1);
        const auto signalled = changed.takeFirst().at(1)
            .value<qiftop::dbus::ConnectionDtoList>();
        QCOMPARE(signalled.size(), 4096);
        QCOMPARE(service.GetConnections().size(), 4096);

        QSet<quint16> keptPorts;
        keptPorts.reserve(signalled.size());
        const qiftop::dbus::ConnectionDto *attributed = nullptr;
        const qiftop::dbus::ConnectionDto *directionalDto = nullptr;
        for (const auto &dto : signalled) {
            keptPorts.insert(dto.localPort);
            if (dto.localPort == attributedPort)
                attributed = &dto;
            if (dto.localPort == ephLow)
                directionalDto = &dto;
        }

        QVERIFY(!keptPorts.contains(generatedPorts[0]));
        QVERIFY(!keptPorts.contains(generatedPorts[1]));
        QVERIFY(keptPorts.contains(attributedPort));
        QVERIFY(keptPorts.contains(ephLow));
        QVERIFY(!resolver.inspectedPorts.contains(generatedPorts[0]));
        QVERIFY(!resolver.inspectedPorts.contains(generatedPorts[1]));

        QVERIFY(attributed != nullptr);
        QCOMPARE(attributed->pid, quint32(4242));
        QCOMPARE(attributed->uid, quint32(1000));
        QCOMPARE(attributed->comm, QStringLiteral("curl"));
        QCOMPARE(attributed->containerRuntime, QStringLiteral("docker"));
        QCOMPARE(attributed->containerId, QStringLiteral("abcdef123456"));
        QCOMPARE(attributed->containerName, QStringLiteral("web"));
        QCOMPARE(attributed->containerChain.size(), 2);
        QCOMPARE(resolver.enrichCalls.value(proc.pid), 1);
        QCOMPARE(resolver.containerCalls.value(proc.pid), 1);
        QCOMPARE(resolver.chainCalls.value(proc.pid), 1);

        QVERIFY(directionalDto != nullptr);
        QCOMPARE(directionalDto->direction, quint8(Direction::Outbound));
    }
};

QTEST_GUILESS_MAIN(TestServices)
#include "test_services.moc"
