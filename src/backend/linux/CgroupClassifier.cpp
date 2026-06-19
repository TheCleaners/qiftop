#include "CgroupClassifier.h"
#include "CgroupParse.h"
#include "ProcSnapshot.h"

#include <QFile>

#include "util/Logging.h"

namespace qiftop::backend::linuximpl {

namespace {
// Resolve a container-cache TTL request against the floor. 0 (or anything
// non-positive) means "preset default"; positive values are clamped up to
// the floor so a runtime hint can't turn this into a /proc/<pid>/cgroup
// blender. Shared by the ctor and the live setTuning() path.
[[nodiscard]] int safeContainerTtlMs(int raw, int defaultMs, int minMs) noexcept
{
    if (raw <= 0) return defaultMs;
    return raw >= minMs ? raw : minMs;
}
} // namespace

CgroupClassifier::CgroupClassifier(const ResolverTuning &tuning)
{
    m_cacheTtlMs = safeContainerTtlMs(tuning.containerCacheMs, kCacheTtlMs, kMinCacheTtlMs);
    m_clock.start();
}

void CgroupClassifier::setTuning(const ResolverTuning &tuning)
{
    std::scoped_lock lock(m_mu);
    m_cacheTtlMs = safeContainerTtlMs(tuning.containerCacheMs, kCacheTtlMs, kMinCacheTtlMs);
}

bool CgroupClassifier::initialize()
{
    QFile self(QStringLiteral("/proc/self/cgroup"));
    if (!self.open(QIODevice::ReadOnly)) {
        qCWarning(lcVerbose) << "CgroupClassifier: cannot open /proc/self/cgroup;"
                                " container attribution disabled";
        return false;
    }
    // Cheap host probe for CRI-O. The cgroupfs cgroup driver produces
    // the SAME path shape for cri-o and containerd, so without an
    // out-of-band hint the leaf is labelled containerd by default
    // (matches Tracee + the broader ecosystem). On cri-o nodes the
    // socket lives at /run/crio/crio.sock; if it exists, we flip the
    // hint so kubepods cgroupfs leaves are labelled cri-o instead.
    // Single stat at startup — no ongoing cost.
    if (QFile::exists(QStringLiteral("/run/crio/crio.sock"))) {
        m_crioPreferred = true;
        qCInfo(lcVerbose) << "CgroupClassifier: detected CRI-O socket, "
                             "kubepods cgroupfs leaf will label as cri-o";
    }
    m_ready = true;
    qCInfo(lcVerbose) << "CgroupClassifier: ready";
    return true;
}

QStringList CgroupClassifier::capabilities() const
{
    if (!m_ready) return {};
    return {
        QStringLiteral("container-attribution"),
        QStringLiteral("container-chain"),
    };
}

QList<ContainerInfo>
CgroupClassifier::resolveContainerChainForPid(qint32 pid)
{
    if (!m_ready || pid <= 0) return {};

    // Snapshot starttime BEFORE any cache hit so we can detect PID
    // reuse and invalidate the stale entry — otherwise a freshly
    // spawned process landing on a recently-dead pid would inherit
    // the dead process's container badge for up to the configured cache TTL.
    const auto stNowOpt = procsnap::pidStartTime(pid);
    if (!stNowOpt.has_value()) return {};  // pid is gone
    const quint64 stNow = *stNowOpt;

    std::scoped_lock lock(m_mu);
    const qint64 now = m_clock.elapsed();
    if (auto it = m_cache.constFind(pid); it != m_cache.constEnd()) {
        if (it->startTime == stNow && now - it->ts < m_cacheTtlMs) {
            return it->chain;
        }
        // Stale (TTL expired OR pid reused) — fall through to refresh.
    }

    QFile f(QStringLiteral("/proc/%1/cgroup").arg(pid));
    QList<ContainerInfo> chain;
    if (f.open(QIODevice::ReadOnly)) {
        // /proc/<pid>/cgroup is typically <512 bytes (v2 unified) and
        // bounded to a few KiB even on busy v1 hosts.
        const QByteArray data = f.read(4096);
        const auto hint = m_crioPreferred
            ? cgroup::CgroupHint::PreferCrio
            : cgroup::CgroupHint::Auto;
        chain = cgroup::classifyProcCgroupChain(QString::fromUtf8(data), hint);
    }
    // If the open failed (race: pid died between starttime read and
    // here), chain stays empty and we cache that — cheap, and the
    // entry will age out quickly anyway.

    if (m_cache.size() >= kCacheMaxItems) m_cache.clear();
    m_cache.insert(pid, { chain, now, stNow });
    return chain;
}

std::optional<ContainerInfo>
CgroupClassifier::resolveContainerForPid(qint32 pid)
{
    const auto chain = resolveContainerChainForPid(pid);
    if (chain.isEmpty()) return std::nullopt;
    return chain.last();
}

} // namespace qiftop::backend::linuximpl
