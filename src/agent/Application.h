#pragma once

#include <QDBusConnection>
#include <QObject>

#include <memory>

#include "Config.h"
#include "IdleManager.h"
#include "backend/ProcessResolver.h"
#include "backend/ResolverDeepWorker.h"

class NetworkMonitor;
class ConnectionMonitor;

namespace qiftop::agent {

class InterfacesService;
class ConnectionsService;
class AttributionHintManager;

// RAII wrapper around the agent's DBus surface — service objects,
// well-known-name acquisition, and IdleManager wiring. Extracted from
// agent/main.cpp so integration tests can spin up the agent in-process
// against a private bus without paying the QProcess + black-box cost.
//
// Lifetime: caller owns the QDBusConnection and the two monitors and
// keeps them alive longer than the Application. start() returns false
// (with errorString() populated) on any registration failure; the
// caller is then free to log + exit. On success the Application owns
// the services and the IdleManager and tears them down (including
// unregistering objects and releasing the bus name) at destruction.
class Application : public QObject {
    Q_OBJECT

public:
    Application(QDBusConnection                                bus,
                NetworkMonitor                                *netMonitor,
                ConnectionMonitor                             *connMonitor,
                IdleManager::Config                            idleCfg,
                std::unique_ptr<backend::ProcessResolver>      resolver,
                backend::AttributionEagerness                  attrMode =
                    backend::AttributionEagerness::Balanced,
                QObject                                       *parent = nullptr);
    ~Application() override;

    // Registers the two service objects, requests the bus name, wires
    // the IdleManager (including bus-side NameOwnerChanged subscription)
    // and kicks the underlying monitors. Returns true on success; on
    // failure errorString() carries the cause and nothing has been
    // registered on the bus.
    [[nodiscard]] bool start();

    // Disclosure policy for GetProcessDetails' privileged fields. Must be set
    // before start() to take effect; defaults to Owner. Kept as a setter (not
    // a ctor arg) so existing constructions stay source-compatible.
    void setProcessDetailsPolicy(const ProcessDetailsPolicy &policy) { m_detailsPolicy = policy; }

    [[nodiscard]] QString errorString() const { return m_lastError; }

    // Accessors for tests / introspection. Pointers stay valid for the
    // lifetime of the Application; never null after a successful start().
    [[nodiscard]] InterfacesService  *interfacesService()  const { return m_ifaceSvc; }
    [[nodiscard]] ConnectionsService *connectionsService() const { return m_connSvc; }
    [[nodiscard]] IdleManager        *idleManager()        const { return m_idle; }
    [[nodiscard]] AttributionHintManager *attributionHintManager() const { return m_attrHints; }
    [[nodiscard]] backend::ProcessResolver *processResolver() const { return m_resolver.get(); }

private:
    QDBusConnection      m_bus;
    NetworkMonitor      *m_netMonitor  = nullptr;
    ConnectionMonitor   *m_connMonitor = nullptr;
    IdleManager::Config  m_idleCfg;
    backend::AttributionEagerness m_attrMode = backend::AttributionEagerness::Balanced;
    ProcessDetailsPolicy m_detailsPolicy;   // default: Owner
    std::unique_ptr<backend::ProcessResolver> m_resolver;

    InterfacesService   *m_ifaceSvc = nullptr;
    ConnectionsService  *m_connSvc  = nullptr;
    IdleManager         *m_idle     = nullptr;
    AttributionHintManager *m_attrHints = nullptr;
    backend::ResolverDeepWorker *m_deepWorker = nullptr;

    bool                 m_nameOwned = false;
    QString              m_lastError;
};

} // namespace qiftop::agent
