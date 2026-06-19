#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>

struct Connection;  // forward-decl breaks the cycle with Connection.h

namespace qiftop::backend {

// Runtime attribution mode selected by the agent config. `Off` is a hard
// kill switch: the factory returns NullProcessResolver and no resolver layer
// is constructed even if individual feature booleans are true.
enum class AttributionEagerness {
    Off = 0,
    Balanced = 1,
    Eager = 2,
};

// Concrete refresh cadences after resolving the eagerness preset plus any
// admin overrides. Process/socket and cgroup caches are separate internally
// because their historical safe defaults differ; the public INI override keeps
// one knob and applies it to both.
struct ResolverTuning {
    int cacheRefreshMs = 1000;
    int containerCacheMs = 2000;
    int netnsRefreshMs = 5000;

    // Async deep-pass budgets (v0.4 §5). Read by the DeepAttributionWorker,
    // ignored by the resolvers themselves (they only honour the refresh
    // intervals above). Kept in the same struct so a single eagerness preset
    // tunes both the resolver cadences and the deep-pass limits.
    int deepQueueMax    = 8192; // bounded queue memory (~2 capped snapshots)
    int deepBatchMax    = 256;  // requests reprocessed per coalesce tick
    int deepCoalesceMs  = 100;  // stream refinements without signal storms
    int deepMaxAttempts = 12;   // age a flow out after N unresolved retries
    // When set, the deep worker asks the resolver to refresh its expensive
    // sources early (Linux: an immediate cross-netns sock_diag sweep) for
    // top-talker flows the cheap retry can't crack. Eager-only by default —
    // demand scans cost setns(2) + per-netns netlink dumps.
    bool deepDemandNetnsScan = false;

    friend bool operator==(const ResolverTuning &, const ResolverTuning &) = default;
};

[[nodiscard]] constexpr ResolverTuning balancedResolverTuning() noexcept
{
    return {};
}

[[nodiscard]] constexpr ResolverTuning eagerResolverTuning() noexcept
{
    return {
        .cacheRefreshMs = 250,
        .containerCacheMs = 1000,
        .netnsRefreshMs = 1000,
        .deepBatchMax   = 2048, // catch up faster on busy hosts
        .deepCoalesceMs = 50,   // stream refinements sooner
        .deepDemandNetnsScan = true, // kick early netns sweeps for misses
    };
}

[[nodiscard]] constexpr ResolverTuning offResolverTuning() noexcept
{
    return {
        .cacheRefreshMs = 0,
        .containerCacheMs = 0,
        .netnsRefreshMs = 0,
    };
}

// Pure preset mapper used by the agent config loader and unit tests.
// Non-zero overrides are assumed to have been range-checked by the caller.
[[nodiscard]] constexpr ResolverTuning
resolverTuningFor(AttributionEagerness eagerness,
                  int cacheRefreshOverrideMs = 0,
                  int netnsRefreshOverrideMs = 0) noexcept
{
    ResolverTuning tuning = balancedResolverTuning();
    switch (eagerness) {
    case AttributionEagerness::Off:
        return offResolverTuning();
    case AttributionEagerness::Balanced:
        tuning = balancedResolverTuning();
        break;
    case AttributionEagerness::Eager:
        tuning = eagerResolverTuning();
        break;
    }

    if (cacheRefreshOverrideMs > 0) {
        tuning.cacheRefreshMs = cacheRefreshOverrideMs;
        tuning.containerCacheMs = cacheRefreshOverrideMs;
    }
    if (netnsRefreshOverrideMs > 0) {
        tuning.netnsRefreshMs = netnsRefreshOverrideMs;
    }
    return tuning;
}

// Stable lowercase wire spelling of an eagerness mode, shared by the agent
// DBus surface (SetDesiredAttributionEagerness / the AttributionEagerness
// property) and the unit tests. Keep these tokens append-only — clients
// branch on the exact strings.
[[nodiscard]] inline QString eagernessToString(AttributionEagerness mode)
{
    switch (mode) {
    case AttributionEagerness::Off:      return QStringLiteral("off");
    case AttributionEagerness::Balanced: return QStringLiteral("balanced");
    case AttributionEagerness::Eager:    return QStringLiteral("eager");
    }
    return QStringLiteral("balanced");
}

// Parse a runtime eagerness token (case-insensitive, surrounding
// whitespace ignored). Recognises `off`/`balanced`/`eager`. Returns
// nullopt for everything else, INCLUDING `default` and the empty string —
// the caller decides what "no concrete mode" means (clear the hint for
// default/empty; ignore unrecognised garbage).
[[nodiscard]] inline std::optional<AttributionEagerness>
eagernessFromString(const QString &s)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("off"))      return AttributionEagerness::Off;
    if (t == QLatin1String("balanced")) return AttributionEagerness::Balanced;
    if (t == QLatin1String("eager"))    return AttributionEagerness::Eager;
    return std::nullopt;
}

// Configuration knobs threaded through from the agent's runtime config
// (`/etc/qiftop/agent.conf`, [attribution]). Defaults match the historical
// production behaviour: balanced cadence, every compiled-in layer enabled.
//
// Each boolean can only override a compile-time-enabled feature OFF at runtime;
// it cannot turn ON code that was not linked into the build.
struct ProcessResolverConfig {
    AttributionEagerness eagerness = AttributionEagerness::Balanced;
    bool processAttribution = true;
    bool containerAttribution = true;
    bool netnsScan = true;
    ResolverTuning tuning = balancedResolverTuning();
};

// Per-process metadata that may be attached to a Connection.
//
// LIFETIME: snapshot value. The resolver caches PID metadata internally but
// hands callers an owned copy so PID reuse between snapshots doesn't tear
// the model.
struct ProcessInfo {
    qint32  pid = 0;
    QString comm;      // short process name (kernel-truncated to 15 bytes)
    QString cmdline;   // full argv joined by spaces
    QString exe;       // resolved /proc/<pid>/exe target (or platform equivalent)
    quint32 uid = 0;

    // A ProcessInfo with pid==0 is the sentinel "not attributed" value.
    // Resolvers MUST set pid > 0 on every meaningful result so consumers
    // can use this flag without inspecting individual fields.
    [[nodiscard]] bool valid() const { return pid > 0; }

    friend bool operator==(const ProcessInfo &, const ProcessInfo &) = default;
};

// Container / cgroup-scope metadata that may be attached to a Connection.
//
// `runtime` is the lowercase runtime name (`"docker"`, `"containerd"`,
// `"podman"`, `"systemd"`, `"lxc"`, ...). `id` is the short, displayable
// identifier — typically the first 12 hex chars of the container ID, or
// the systemd unit name (`"unit:foo.service"`) for non-container scopes.
// `name` is the friendly name (e.g. `"happy_einstein"`); empty when the
// resolver wasn't asked to resolve names or doesn't know how.
struct ContainerInfo {
    QString runtime;
    QString id;
    QString name;

    [[nodiscard]] bool valid() const { return !runtime.isEmpty() && !id.isEmpty(); }

    friend bool operator==(const ContainerInfo &, const ContainerInfo &) = default;
};

// Abstract per-flow attribution resolver.
//
// Implementations live in `backend/<platform>/` (linux: SockDiag + Cgroup +
// Netns) or `backend/null/` (universal no-op fallback). Each resolver
// composes one or more underlying data sources and advertises what it
// actually has at runtime via capabilities().
//
// THREADING
//   resolve*() may be called from the agent's ConntrackMonitor worker
//   thread on every conntrack snapshot. Implementations MUST be
//   reentrant-safe across resolvePid / enrichPid /
//   resolveContainerForPid calls and should maintain bounded caches with
//   a short TTL (~1s) since the same PIDs and containers are queried
//   repeatedly.
//
// CAPABILITIES
//   Tokens use the same dash-separated lowercase grammar as the
//   InterfacesService::capabilities() list. v0.2 introduces:
//     "process-attribution"   — resolvePid/enrichPid return useful ProcessInfo
//     "container-attribution" — resolveContainerForPid returns useful ContainerInfo
//     "netns-scan"            — process attribution sees container netns flows
//   A resolver MUST omit a token when the underlying API isn't reachable
//   at runtime (e.g. kernel doesn't support NETLINK_SOCK_DIAG, /proc is
//   unreadable, etc.) even if the code path was compiled in.
class ProcessResolver {
public:
    virtual ~ProcessResolver() = default;

    // One-shot startup probe. Implementations open sockets, read sysfs
    // probes, etc., and return true if any portion of the resolver
    // became usable. Returning false does NOT abort startup — the
    // caller treats it as "advertise no tokens, pass through to Null
    // semantics" — but is logged.
    virtual bool initialize() = 0;

    // Runtime detected capability tokens. Subset of the compile-time
    // feature set, intersected with whatever the runtime probe found.
    [[nodiscard]] virtual QStringList capabilities() const = 0;

    // Runtime re-tune hook. Called when the agent's effective attribution
    // eagerness changes at runtime (a client sent
    // SetDesiredAttributionEagerness over DBus, or an existing hint
    // expired). Default no-op: resolvers with no tunable refresh cadence
    // (NullResolver, BSD socket resolver) simply ignore it. Concrete Linux
    // resolvers update their refresh-cadence member(s) live and clamp to
    // their existing floors; CompositeResolver fans out to its children.
    //
    // THREADING: invoked from the agent main thread, while resolve*() may be
    // running on a worker thread. Implementations that already guard their
    // cadence members with a mutex MUST take the same lock here; otherwise a
    // single relaxed int write is acceptable (a torn read of an int cadence
    // is benign — worst case one stale refresh interval). See AGENTS.md §4.
    virtual void setTuning(const ResolverTuning &) {}

    // Demand-driven deep scan hook (v0.4 §5). The async deep-pass worker calls
    // this when a weakly-attributed top-talker flow has resisted the cheap
    // cache-backed retries, asking the resolver to refresh its EXPENSIVE data
    // sources early instead of waiting for the next periodic tick. The only
    // current implementor is the Linux NetnsScanner, which kicks an immediate
    // (rate-limited) cross-netns sock_diag sweep on its own worker thread so a
    // container flow whose socket lives in a not-yet-scanned netns attributes
    // sooner. Default no-op: resolvers with no expensive periodic source (host
    // sock_diag, cgroup, BSD, Null) ignore it. CompositeResolver fans out.
    //
    // THREADING: invoked from the agent main thread (the deep worker lives
    // there); implementations MUST NOT block — NetnsScanner just posts a queued
    // tick to its worker thread and returns. Safe to call frequently; callees
    // rate-limit so a burst of unresolved flows can't storm the scanner.
    virtual void requestDeepScan() {}

    // Cheaply look up the PID owning the local end of `flow`. Returns 0
    // when the resolver cannot attribute (flow originates from a netns it
    // doesn't scan, is purely forwarded, or the underlying socket has
    // already closed). Implementations should avoid expensive /proc
    // enrichment here so callers can memoise enrichPid() per unique PID.
    [[nodiscard]] virtual qint32 resolvePid(const Connection &flow) = 0;

    // Enrich a PID with process metadata. This is where /proc reads
    // happen; callers that batch flow snapshots should call it once per
    // unique PID. Implementations MUST guard PID reuse (e.g. via
    // starttime) before returning data.
    [[nodiscard]] virtual std::optional<ProcessInfo>
        enrichPid(qint32 pid) = 0;

    // Convenience API for direct callers. Batch code should prefer
    // resolvePid() + per-PID enrichPid() memoisation.
    [[nodiscard]] virtual std::optional<ProcessInfo>
        resolveFlow(const Connection &flow)
    {
        const qint32 pid = resolvePid(flow);
        if (pid <= 0) return std::nullopt;
        return enrichPid(pid);
    }

    // Look up container metadata for a PID. Returns nullopt for
    // host-native processes (no container scope) or when container
    // attribution is disabled.
    [[nodiscard]] virtual std::optional<ContainerInfo>
        resolveContainerForPid(qint32 pid) = 0;

    // Look up the FULL container nesting chain for a PID, ordered
    // OUTERMOST → INNERMOST. For a pod running in a k3d node this
    // returns e.g. [docker:k3d-node, kubernetes:pod, containerd:workload];
    // for a plain docker container it returns [docker:id]; for a host
    // process it returns an empty list.
    //
    // Default implementation: wrap `resolveContainerForPid` into a
    // single-element list (i.e. "I only know the innermost"). Override
    // when the underlying data source can see the whole chain (the
    // cgroup classifier does, since /proc/<pid>/cgroup carries the
    // full path).
    //
    // Resolvers that override this MUST advertise the
    // `container-chain` capability token; consumers branch on token
    // presence, not on Qt version or vtable shape.
    [[nodiscard]] virtual QList<ContainerInfo>
        resolveContainerChainForPid(qint32 pid)
    {
        if (auto ci = resolveContainerForPid(pid)) return { *ci };
        return {};
    }
};

} // namespace qiftop::backend

// Lets AttributionEagerness ride Qt signals/slots and be inspected via
// QSignalSpy (AttributionHintManager::effectiveModeChanged) and queued
// connections. Harmless for the Widgets-free libqiftop surface (Qt Core).
Q_DECLARE_METATYPE(qiftop::backend::AttributionEagerness)
