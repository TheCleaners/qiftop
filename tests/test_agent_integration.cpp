// Integration test for qiftop-agent.
//
// Spawns the actual ./qiftop-agent binary against a session bus (provided
// by `dbus-run-session` in CI, or the user's own session bus when running
// locally with `cmake --build build && ctest`) and exercises the live
// DBus contract:
//
//   • org.qiftop.NetworkAgent1 registers within a few seconds
//   • Properties.Get(Version)      → "0.6"
//   • Properties.Get(Capabilities) → contains the stable contract tokens
//   • GetInterfaces() returns quickly without error
//   • SetDesiredIntervalMs(200) raises StatsChanged emission rate well
//     above the 1 Hz default
//
// All assertions complete in well under 10 seconds. The agent is killed
// in cleanup() so a failed assertion can't leak a process across runs.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTest>

#include <unistd.h>  // getpid for the GetProcessDetails round-trip test

#ifndef QIFTOP_AGENT_BINARY
#  error "QIFTOP_AGENT_BINARY must be defined by CMake (path to qiftop-agent)"
#endif

namespace {

constexpr auto kBusName     = "org.qiftop.NetworkAgent1";
constexpr auto kIfacesPath  = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kIfacesIface = "org.qiftop.NetworkAgent1.Interfaces";

bool waitForNameRegistered(QDBusConnection &bus, const QString &name, int timeoutMs)
{
    auto *ifc = bus.interface();
    if (!ifc) return false;
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeoutMs) {
        if (ifc->isServiceRegistered(name)) return true;
        QTest::qWait(50);
    }
    return false;
}

} // namespace

// File-scope counter QObject so moc picks up the slot. Used to count
// StatsChanged signal emissions without caring about the payload.
class SignalCounter : public QObject {
    Q_OBJECT
public:
    int count = 0;
public slots:
    void bump() { ++count; }
};

class TestAgentIntegration : public QObject {
    Q_OBJECT

private:
    QProcess m_proc;

    void spawnAgent()
    {
        m_proc.setProcessChannelMode(QProcess::ForwardedChannels);
        m_proc.start(QString::fromLatin1(QIFTOP_AGENT_BINARY),
                     {QStringLiteral("--session"), QStringLiteral("--verbose")});
        QVERIFY2(m_proc.waitForStarted(2000),
                 qPrintable(QStringLiteral("agent failed to start: %1")
                                .arg(m_proc.errorString())));
        auto bus = QDBusConnection::sessionBus();
        QVERIFY2(waitForNameRegistered(bus, QString::fromLatin1(kBusName), 5000),
                 "agent did not register its bus name within 5 s");
    }

private slots:
    void initTestCase()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("No session bus available — run under dbus-run-session(1).");

        const QFileInfo bin(QString::fromLatin1(QIFTOP_AGENT_BINARY));
        QVERIFY2(bin.exists() && bin.isExecutable(),
                 qPrintable(QStringLiteral("qiftop-agent binary not found at %1")
                                .arg(bin.absoluteFilePath())));

        if (QDBusConnection::sessionBus().interface()->isServiceRegistered(
                QString::fromLatin1(kBusName))) {
            QSKIP("org.qiftop.NetworkAgent1 already registered on this session "
                  "bus — cannot integration-test cleanly.");
        }
    }

    void init()  { spawnAgent(); }

    void cleanup()
    {
        if (m_proc.state() != QProcess::NotRunning) {
            m_proc.terminate();
            if (!m_proc.waitForFinished(2000)) m_proc.kill();
            m_proc.waitForFinished(1000);
        }
        // Give the bus a moment to release the well-known name before the
        // next test re-acquires it.
        QTest::qWait(150);
    }

    void versionAndCapabilities()
    {
        auto bus = QDBusConnection::sessionBus();
        QDBusInterface props(QString::fromLatin1(kBusName),
                             QString::fromLatin1(kIfacesPath),
                             QStringLiteral("org.freedesktop.DBus.Properties"),
                             bus);
        QVERIFY(props.isValid());

        QDBusReply<QDBusVariant> vReply = props.call(
            QStringLiteral("Get"),
            QString::fromLatin1(kIfacesIface),
            QStringLiteral("Version"));
        QVERIFY2(vReply.isValid(), qPrintable(vReply.error().message()));
        QCOMPARE(vReply.value().variant().toString(), QStringLiteral("0.7"));

        QDBusReply<QDBusVariant> cReply = props.call(
            QStringLiteral("Get"),
            QString::fromLatin1(kIfacesIface),
            QStringLiteral("Capabilities"));
        QVERIFY2(cReply.isValid(), qPrintable(cReply.error().message()));
        const QStringList caps = cReply.value().variant().toStringList();
        // Stable contract tokens — never rename, only add. If any of these
        // disappears the contract is broken and older clients will silently
        // stop gating optional behaviour.
        for (const auto *tok : {"cadence-hints", "cadence-signal",
                                "name-owner-cleanup", "monotonic-clock",
                                "snapshot-cap", "iana-proto",
                                "direction-on-wire", "snapshot-timestamp",
                                "ifindex", "oper-state", "link-errors",
                                "tcp-state", "attribution-reason",
                                "on-demand-process-details",
                                "attribution-eagerness-hints"}) {
            QVERIFY2(caps.contains(QString::fromLatin1(tok)),
                     qPrintable(QStringLiteral("missing capability '%1'; got: [%2]")
                                    .arg(QString::fromLatin1(tok),
                                         caps.join(QStringLiteral(", ")))));
        }
    }

    void getInterfacesReturnsQuickly()
    {
        auto bus = QDBusConnection::sessionBus();
        QDBusInterface ifc(QString::fromLatin1(kBusName),
                           QString::fromLatin1(kIfacesPath),
                           QString::fromLatin1(kIfacesIface),
                           bus);
        QVERIFY(ifc.isValid());

        QElapsedTimer t; t.start();
        QDBusMessage reply = ifc.call(QStringLiteral("GetInterfaces"));
        QVERIFY2(reply.type() != QDBusMessage::ErrorMessage,
                 qPrintable(reply.errorMessage()));
        QVERIFY2(t.elapsed() < 1000, "GetInterfaces took longer than 1 s");
    }

    void desiredIntervalAcceleratesStatsChanged()
    {
        auto bus = QDBusConnection::sessionBus();

        SignalCounter counter;
        const bool subscribed = bus.connect(
            QString::fromLatin1(kBusName),
            QString::fromLatin1(kIfacesPath),
            QString::fromLatin1(kIfacesIface),
            QStringLiteral("StatsChanged"),
            &counter, SLOT(bump()));
        QVERIFY2(subscribed, "failed to subscribe to StatsChanged");

        // Establish a baseline at the default cadence (~1 Hz).
        QTest::qWait(1500);
        const int baseline = counter.count;

        // Request a fast cadence; measure over a 2 s window to absorb jitter.
        QDBusInterface ifaceProxy(QString::fromLatin1(kBusName),
                                  QString::fromLatin1(kIfacesPath),
                                  QString::fromLatin1(kIfacesIface),
                                  bus);
        QDBusMessage setReply = ifaceProxy.call(
            QStringLiteral("SetDesiredIntervalMs"), quint32(200));
        QVERIFY2(setReply.type() != QDBusMessage::ErrorMessage,
                 qPrintable(setReply.errorMessage()));

        const int beforeFast = counter.count;
        QTest::qWait(2000);
        const int fastTicks = counter.count - beforeFast;

        // At 200 ms we expect ~10 ticks in 2 s. Allow generous headroom for
        // CI jitter (scheduling, DBus delivery latency): assert ≥ 5.
        QVERIFY2(fastTicks >= 5,
                 qPrintable(QStringLiteral("expected ≥5 StatsChanged in 2 s "
                                           "under 200 ms hint; got %1 "
                                           "(baseline before hint: %2)")
                                .arg(fastTicks).arg(baseline)));
        QVERIFY2(fastTicks > baseline,
                 qPrintable(QStringLiteral("fast=%1 should exceed baseline=%2")
                                .arg(fastTicks).arg(baseline)));

        bus.disconnect(QString::fromLatin1(kBusName),
                       QString::fromLatin1(kIfacesPath),
                       QString::fromLatin1(kIfacesIface),
                       QStringLiteral("StatsChanged"),
                       &counter, SLOT(bump()));
    }

    void getProcessDetailsReturnsOurOwnInfo()
    {
        // Drive the v0.5 on-demand RPC. The agent runs as the test user
        // under dbus-run-session, so it can introspect this test
        // process itself: pid=getpid() must round-trip cleanly with a
        // non-empty cmdline (which definitely contains "test_agent…").
        auto bus = QDBusConnection::sessionBus();
        QDBusInterface conns(QString::fromLatin1(kBusName),
                             QStringLiteral("/org/qiftop/NetworkAgent1/Connections"),
                             QStringLiteral("org.qiftop.NetworkAgent1.Connections"),
                             bus);
        QVERIFY(conns.isValid());

        // Unmarshal the (uussssτ) return struct manually — keeps the test
        // free of any client-side header coupling. Calling QDBusInterface
        // with a typed return would force linking the ProcessDetailsDto
        // metatype into this TU, which we deliberately avoid (the test
        // is meant to verify the WIRE, not the C++ DTO mirror).
        QDBusMessage reply = conns.call(QStringLiteral("GetProcessDetails"),
                                        quint32(::getpid()));
        QVERIFY2(reply.type() != QDBusMessage::ErrorMessage,
                 qPrintable(reply.errorMessage()));

        const QVariantList args = reply.arguments();
        QCOMPARE(args.size(), 1);
        const auto v = args.first();
        // QDBusArgument round-trip into our 7 fields.
        const QDBusArgument arg = v.value<QDBusArgument>();
        quint32 pid = 0, uid = 0;
        QString comm, exe, cmdline, cwd;
        quint64 startTime = 0;
        arg.beginStructure();
        arg >> pid >> uid >> comm >> exe >> cmdline >> cwd >> startTime;
        arg.endStructure();

        QCOMPARE(pid, quint32(::getpid()));
        // comm is kernel-truncated to 15 bytes; this binary's basename
        // ("test_agent_integration") is exactly 22 chars and will be
        // truncated to "test_agent_inte".
        QVERIFY2(!comm.isEmpty(), qPrintable(comm));
        // cmdline should at least contain our binary path or basename.
        QVERIFY2(cmdline.contains(QStringLiteral("test_agent_integration")),
                 qPrintable(cmdline));
        QVERIFY2(!exe.isEmpty(), qPrintable(exe));
        QVERIFY2(startTime > 0,
                 qPrintable(QStringLiteral("startTimeJiffies=%1").arg(startTime)));
    }

    void getProcessDetailsOnVanishedPidReturnsZeroPid()
    {
        // Asking for a PID that is certain not to exist (UINT32 - 1
        // gets very close to PID_MAX). Spec: pid=0 in the response,
        // method must NOT raise a DBus error.
        auto bus = QDBusConnection::sessionBus();
        QDBusInterface conns(QString::fromLatin1(kBusName),
                             QStringLiteral("/org/qiftop/NetworkAgent1/Connections"),
                             QStringLiteral("org.qiftop.NetworkAgent1.Connections"),
                             bus);
        QDBusMessage reply = conns.call(QStringLiteral("GetProcessDetails"),
                                        quint32(2147483646));
        QVERIFY2(reply.type() != QDBusMessage::ErrorMessage,
                 qPrintable(reply.errorMessage()));
        const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
        quint32 pid = 99, uid = 99;
        QString comm, exe, cmdline, cwd;
        quint64 startTime = 99;
        arg.beginStructure();
        arg >> pid >> uid >> comm >> exe >> cmdline >> cwd >> startTime;
        arg.endStructure();
        QCOMPARE(pid, quint32(0));
        QVERIFY(comm.isEmpty());
        QVERIFY(cmdline.isEmpty());
    }

    void attributionEagernessHintRoundTrips()
    {
        // Drive the v0.7 runtime attribution-eagerness override over the bus:
        // SetDesiredAttributionEagerness("eager") must return the new effective
        // mode, and the AttributionEagerness property must reflect it.
        auto bus = QDBusConnection::sessionBus();
        const auto connsPath  = QStringLiteral("/org/qiftop/NetworkAgent1/Connections");
        const auto connsIface = QStringLiteral("org.qiftop.NetworkAgent1.Connections");

        QDBusInterface conns(QString::fromLatin1(kBusName), connsPath, connsIface, bus);
        QVERIFY(conns.isValid());

        QDBusReply<QString> setReply = conns.call(
            QStringLiteral("SetDesiredAttributionEagerness"), QStringLiteral("eager"));
        QVERIFY2(setReply.isValid(), qPrintable(setReply.error().message()));
        QCOMPARE(setReply.value(), QStringLiteral("eager"));

        // Property mirrors the effective mode.
        QDBusInterface props(QString::fromLatin1(kBusName), connsPath,
                             QStringLiteral("org.freedesktop.DBus.Properties"), bus);
        QVERIFY(props.isValid());
        QDBusReply<QDBusVariant> pReply = props.call(
            QStringLiteral("Get"), connsIface, QStringLiteral("AttributionEagerness"));
        QVERIFY2(pReply.isValid(), qPrintable(pReply.error().message()));
        QCOMPARE(pReply.value().variant().toString(), QStringLiteral("eager"));

        // Clearing this client's hint reverts to the agent's config default
        // (balanced out of the box).
        QDBusReply<QString> clearReply = conns.call(
            QStringLiteral("SetDesiredAttributionEagerness"), QStringLiteral("default"));
        QVERIFY2(clearReply.isValid(), qPrintable(clearReply.error().message()));
        QCOMPARE(clearReply.value(), QStringLiteral("balanced"));

        // An unrecognised mode is ignored and reports the current effective.
        QDBusReply<QString> junkReply = conns.call(
            QStringLiteral("SetDesiredAttributionEagerness"), QStringLiteral("turbo"));
        QVERIFY2(junkReply.isValid(), qPrintable(junkReply.error().message()));
        QCOMPARE(junkReply.value(), QStringLiteral("balanced"));
    }
};

QTEST_MAIN(TestAgentIntegration)
#include "test_agent_integration.moc"
