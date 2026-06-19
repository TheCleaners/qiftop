#include "Application.h"

#include <QDBusError>

#include "ConnectionsService.h"
#include "InterfacesService.h"
#include "AttributionHintManager.h"
#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"
#include "util/Logging.h"

namespace qiftop::agent {

namespace {
constexpr auto kBusName    = "org.qiftop.NetworkAgent1";
constexpr auto kIfacesPath = "/org/qiftop/NetworkAgent1/Interfaces";
constexpr auto kConnsPath  = "/org/qiftop/NetworkAgent1/Connections";
} // namespace

Application::Application(QDBusConnection                                bus,
                         NetworkMonitor                                *netMonitor,
                         ConnectionMonitor                             *connMonitor,
                         IdleManager::Config                            idleCfg,
                         std::unique_ptr<backend::ProcessResolver>      resolver,
                         backend::AttributionEagerness                  attrMode,
                         QObject                                       *parent)
    : QObject(parent)
    , m_bus(std::move(bus))
    , m_netMonitor(netMonitor)
    , m_connMonitor(connMonitor)
    , m_idleCfg(idleCfg)
    , m_attrMode(attrMode)
    , m_resolver(std::move(resolver))
{}

Application::~Application()
{
    // Best-effort teardown so a re-construction against the same bus
    // (notably the integration tests) doesn't trip over leftover state.
    if (m_nameOwned)
        m_bus.unregisterService(QString::fromLatin1(kBusName));
    if (m_ifaceSvc)
        m_bus.unregisterObject(QString::fromLatin1(kIfacesPath));
    if (m_connSvc)
        m_bus.unregisterObject(QString::fromLatin1(kConnsPath));
    delete m_attrHints;
    delete m_idle;
    delete m_connSvc;
    delete m_ifaceSvc;
}

bool Application::start()
{
    if (!m_bus.isConnected()) {
        m_lastError = m_bus.lastError().message();
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral("DBus connection is not open");
        return false;
    }

    m_ifaceSvc = new InterfacesService(m_netMonitor);
    m_ifaceSvc->setProcessResolver(m_resolver.get());
    m_connSvc  = new ConnectionsService(m_connMonitor);
    m_connSvc->setProcessDetailsPolicy(m_detailsPolicy);
    if (m_resolver) {
        // `container-chain-wire` is gated on the resolver's own
        // `container-chain` capability; mirror that here so we don't
        // call resolveContainerChainForPid (cheap, but pointless) when
        // the resolver only ever returns single-entry chains via the
        // default base impl.
        const bool wantChain =
            m_resolver->capabilities().contains(QStringLiteral("container-chain"));
        m_connSvc->setProcessResolver(m_resolver.get(), wantChain);
    }

    // Register the service objects *before* requesting the bus name so that
    // a client triggered by DBus activation always finds them in place.
    constexpr auto kRegisterOpts = QDBusConnection::ExportAllContents;
    if (!m_bus.registerObject(QString::fromLatin1(kIfacesPath), m_ifaceSvc, kRegisterOpts)) {
        m_lastError = QStringLiteral("registerObject(%1) failed: %2")
                          .arg(QString::fromLatin1(kIfacesPath),
                               m_bus.lastError().message());
        return false;
    }
    if (!m_bus.registerObject(QString::fromLatin1(kConnsPath), m_connSvc, kRegisterOpts)) {
        m_lastError = QStringLiteral("registerObject(%1) failed: %2")
                          .arg(QString::fromLatin1(kConnsPath),
                               m_bus.lastError().message());
        return false;
    }
    if (!m_bus.registerService(QString::fromLatin1(kBusName))) {
        m_lastError = QStringLiteral("registerService(%1) failed: %2")
                          .arg(QString::fromLatin1(kBusName),
                               m_bus.lastError().message());
        return false;
    }
    m_nameOwned = true;

    qCInfo(lcVerbose).noquote() << "agent: bus name acquired" << kBusName;

    m_idle = new IdleManager(m_netMonitor, m_connMonitor, m_idleCfg);
    m_idle->attachBus(m_bus); // drop hints immediately on peer disconnect
    m_ifaceSvc->setIdleManager(m_idle);
    m_connSvc->setIdleManager(m_idle);

    // Runtime attribution-eagerness override (capability:
    // attribution-eagerness-hints). Starts at the config default; clients
    // can raise/lower it at runtime via SetDesiredAttributionEagerness. The
    // hint TTL matches the cadence hint TTL so a client can reuse one
    // heartbeat for both. On every effective-mode change the ConnectionsService
    // re-tunes the live resolver and mirrors AttributionEagernessChanged.
    m_attrHints = new AttributionHintManager(m_attrMode, m_idleCfg.hintTtlMs);
    m_attrHints->attachBus(m_bus);
    m_connSvc->setAttributionHintManager(m_attrHints);

    if (m_netMonitor)  m_netMonitor->start();
    if (m_connMonitor) m_connMonitor->start();
    m_idle->noteActivity(); // ensure we start at the active interval
    return true;
}

} // namespace qiftop::agent
