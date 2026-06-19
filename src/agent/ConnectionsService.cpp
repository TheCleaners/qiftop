#include "ConnectionsService.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>

#include <algorithm>
#include <limits>
#include <vector>

#include "IdleManager.h"
#include "AttributionHintManager.h"
#include "backend/Attribution.h"
#include "backend/ConnectionMonitor.h"
#include "backend/PlatformInfo.h"
#include "util/ConnectionHeuristics.h"
#include "util/Logging.h"

#ifdef BACKEND_LINUX
#include "backend/linux/ProcDetails.h"
#endif

namespace qiftop::agent {

namespace {

// Snapshot of "what counts as this host" for direction inference.
// Refreshed every tick because addresses can come and go (DHCP renew,
// VPN up/down, container creation).
struct HostContext {
    QSet<QHostAddress> localAddrs;
    QSet<QHostAddress> loopbackAddrs;
    quint16            ephemeralLow  = 32768;
    quint16            ephemeralHigh = 60999;
};

HostContext gatherHostContext()
{
    HostContext ctx;
    ctx.localAddrs    = qiftop::platform::localAddresses();
    ctx.loopbackAddrs = qiftop::platform::loopbackAddresses();
    const auto [lo, hi] = qiftop::platform::ephemeralPortRange();
    ctx.ephemeralLow  = lo;
    ctx.ephemeralHigh = hi;
    return ctx;
}

struct ByteTotal {
    quint64 low = 0;
    bool carry = false;
};

ByteTotal totalBytes(const Connection &c)
{
    constexpr auto kMax = std::numeric_limits<quint64>::max();
    return ByteTotal{
        c.rxBytes + c.txBytes,
        c.rxBytes > kMax - c.txBytes,
    };
}

bool hasGreaterTotalBytes(const Connection &a, const Connection &b)
{
    const ByteTotal at = totalBytes(a);
    const ByteTotal bt = totalBytes(b);
    if (at.carry != bt.carry)
        return at.carry;
    return at.low > bt.low;
}

} // namespace

ConnectionsService::ConnectionsService(ConnectionMonitor *monitor, QObject *parent)
    : QObject(parent)
    , m_monitor(monitor)
{
    m_clock.start();
    connect(m_monitor, &ConnectionMonitor::connectionsUpdated,
            this,      &ConnectionsService::onConnectionsUpdated);
    connect(m_monitor, &ConnectionMonitor::permissionDenied,
            this,      &ConnectionsService::onPermissionDenied);
    connect(m_monitor, &ConnectionMonitor::accountingUnavailable,
            this,      &ConnectionsService::onAccountingUnavailable);
}

dbus::ConnectionDtoList ConnectionsService::GetConnections()
{
    if (m_idle) m_idle->noteActivity();
    return m_last;
}

void ConnectionsService::SetDesiredIntervalMs(uint intervalMs)
{
    if (!m_idle) return;
    const QString sender = calledFromDBus() ? message().service() : QString();
    // See ConnectionsService::SetDesiredIntervalMs — only count accepted
    // hints as activity (see commit message for rationale).
    if (m_idle->setClientHint(sender, static_cast<int>(intervalMs)))
        m_idle->noteActivity();
}

QString ConnectionsService::attributionEagerness() const
{
    return m_attrHints ? m_attrHints->effectiveModeString()
                       : backend::eagernessToString(backend::AttributionEagerness::Balanced);
}

void ConnectionsService::setAttributionHintManager(AttributionHintManager *mgr)
{
    m_attrHints = mgr;
    if (m_attrHints) {
        connect(m_attrHints, &AttributionHintManager::effectiveModeChanged,
                this,        &ConnectionsService::onEffectiveEagernessChanged);
    }
}

QString ConnectionsService::SetDesiredAttributionEagerness(const QString &mode)
{
    if (!m_attrHints)
        return attributionEagerness();   // no manager wired (bare unit test)

    const QString sender = calledFromDBus() ? message().service() : QString();
    const QString trimmed = mode.trimmed();

    // `default`/empty → clear this client's hint.
    bool accepted = false;
    if (trimmed.isEmpty() || trimmed.compare(QLatin1String("default"),
                                             Qt::CaseInsensitive) == 0) {
        accepted = m_attrHints->clearHint(sender);
    } else if (const auto parsed = backend::eagernessFromString(trimmed)) {
        accepted = m_attrHints->setHint(sender, *parsed);
    } else {
        // Unrecognised token: ignore it (record nothing), just report the
        // current effective mode back. No sendErrorReply — keeping the call
        // total (always returns a string) is friendlier to scripting clients.
        qCInfo(lcVerbose).noquote()
            << "SetDesiredAttributionEagerness: ignoring unrecognised mode"
            << mode << "from" << sender;
    }

    // Anti-abuse, mirroring the cadence hints: only an ACCEPTED hint counts
    // as activity, so a peer rejected from the (capped) hint table can't pin
    // the agent out of idle by hammering this method.
    if (accepted && m_idle)
        m_idle->noteActivity();

    return m_attrHints->effectiveModeString();
}

void ConnectionsService::onEffectiveEagernessChanged(backend::AttributionEagerness mode)
{
    // Re-tune the live resolver to the new effective cadence. We never tear
    // down / rebuild the resolver at runtime (too heavy + racy across the
    // worker threads): a runtime `off` hint just quiesces refresh via the
    // off-preset cadences, whereas a config `eagerness=off` builds a
    // NullResolver at startup. That asymmetry is intentional and documented
    // (AGENTS.md §4): runtime-off is "stop refreshing", not "tear down".
    if (m_resolver)
        m_resolver->setTuning(backend::resolverTuningFor(mode));
    emit AttributionEagernessChanged(backend::eagernessToString(mode));
}

dbus::ProcessDetailsDto ConnectionsService::GetProcessDetails(uint pid)
{
    if (m_idle) m_idle->noteActivity();
    dbus::ProcessDetailsDto out;
    if (pid == 0 || pid > quint32(std::numeric_limits<qint32>::max()))
        return out;

#ifdef BACKEND_LINUX
    const auto d = backend::linux_::readProcessDetails(static_cast<qint32>(pid));
    if (!d.valid) return out;
    // Low-sensitivity bulk fields (already exposed via GetConnections) go to
    // every netdev caller. The privileged symlink/argv fields (exe/cwd/cmdline)
    // — readable only because the root agent holds CAP_SYS_PTRACE /
    // CAP_DAC_READ_SEARCH, and able to leak cross-UID paths and secrets passed
    // on the command line — are disclosed only to root or the PID's owner.
    out.pid              = quint32(d.pid);
    out.uid              = d.uid;
    out.comm             = d.comm;
    out.startTimeJiffies = d.startTimeJiffies;
    if (callerMaySeeProcessFields(d.uid)) {
        out.exe          = d.exe;
        out.cmdline      = d.cmdline;
        out.cwd          = d.cwd;
    }
#else
    Q_UNUSED(pid);
#endif
    return out;
}

bool ConnectionsService::callerMaySeeProcessFields(quint32 targetUid) const
{
    using Mode = ProcessDetailsPolicy::Mode;
    // Permissive: any authorised (netdev) caller — restores pre-0.2.1 behaviour.
    if (m_detailsPolicy.mode == Mode::Permissive)
        return true;
    // Not a D-Bus call (in-process embedding / unit test): the caller is the
    // agent itself — no privilege boundary to enforce.
    if (!calledFromDBus())
        return true;
    auto *iface = connection().interface();
    if (!iface)
        return false;                       // fail safe: can't verify the caller
    const QDBusReply<uint> uidReply = iface->serviceUid(message().service());
    if (!uidReply.isValid())
        return false;                       // fail safe
    const uint caller = uidReply.value();
    // Root and the process owner always see the privileged fields.
    if (caller == 0 || caller == targetUid)
        return true;
    // Restricted: additionally honour the admin-configured user/group allowlist
    // (e.g. wheel) for cross-UID disclosure.
    if (m_detailsPolicy.mode == Mode::Restricted) {
        const QString name = qiftop::platform::userNameForUid(caller);
        if (!name.isEmpty() && m_detailsPolicy.allowUsers.contains(name, Qt::CaseInsensitive))
            return true;
        for (const QString &g : m_detailsPolicy.allowGroups)
            if (qiftop::platform::userInGroup(caller, g))
                return true;
    }
    return false;
}

void ConnectionsService::onConnectionsUpdated(const QList<Connection> &conns)
{
    // Cap the per-tick snapshot size. On a busy router the conntrack
    // table can hold 100 k+ flows; serialising that into a multi-MB DBus
    // message every tick costs both sides real CPU and memory (and m_last
    // pins the high-water mark for the life of the process). Keep the
    // top N by total bytes — those are what the user actually wants to
    // see in a "top talkers" tool — and log when we truncate.
    static constexpr int kMaxConnections = 4096;

    QList<Connection> kept;
    if (conns.size() > kMaxConnections) {
        std::vector<const Connection *> top;
        top.reserve(static_cast<std::size_t>(conns.size()));
        for (const Connection &c : conns)
            top.push_back(&c);
        std::partial_sort(top.begin(),
                          top.begin() + kMaxConnections,
                          top.end(),
                          [](const Connection *a, const Connection *b) {
                              return hasGreaterTotalBytes(*a, *b);
                          });
        kept.reserve(kMaxConnections);
        for (int i = 0; i < kMaxConnections; ++i)
            kept.append(*top[static_cast<std::size_t>(i)]);
        kept.resize(kMaxConnections);
        // qWarning, not qCInfo, because it means the user is losing data.
        qWarning().noquote()
            << "ConnectionsService: capping snapshot at" << kMaxConnections
            << "of" << conns.size() << "flows (kept top talkers by bytes)";
    } else {
        kept = conns;
    }

    // Populate Direction server-side so non-Qt libqiftop consumers don't
    // have to reimplement the heuristic. Done AFTER truncation so we
    // only pay for flows we'll actually ship.
    const auto ctx = gatherHostContext();
    for (auto &c : kept) {
        c.direction = heuristics::inferDirection(
            c, ctx.localAddrs, ctx.loopbackAddrs,
            ctx.ephemeralLow, ctx.ephemeralHigh);
    }

    // Populate process + container attribution from the wired resolver.
    // No-op when resolver is null or returns nothing useful. Socket PID
    // resolution is per-flow, but /proc enrichment and container/chain
    // lookups are memoised by PID in backend::attributeFlows.
    backend::attributeFlows(kept, m_resolver, backend::AttributionOptions{m_wantContainerChain});

    // Explain each flow's attribution outcome (Resolved / Forwarded /
    // Orphaned / NoLocalSocket) once, server-side, reusing the same host
    // context as direction inference. Lets every consumer distinguish "no
    // local process by design" (routed/NAT) from a genuine attribution miss.
    for (auto &c : kept)
        c.reason = heuristics::attributionReason(
            c, ctx.localAddrs, ctx.loopbackAddrs);

    m_last = dbus::toDtos(kept);
    emit ConnectionsChanged(static_cast<qulonglong>(m_clock.elapsed()), m_last);
}

void ConnectionsService::onPermissionDenied(const QString &detail)
{
    emit PermissionDenied(detail);
}

void ConnectionsService::onAccountingUnavailable(const QString &detail)
{
    if (m_accountingEnabled) {
        m_accountingEnabled = false;
        emit AccountingChanged(false);
    }
    // detail is informational only; PermissionDenied is the actionable signal.
    Q_UNUSED(detail);
}

} // namespace qiftop::agent
