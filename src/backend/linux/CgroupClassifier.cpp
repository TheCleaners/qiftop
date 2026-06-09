#include "CgroupClassifier.h"
#include "CgroupParse.h"
#include "ProcSnapshot.h"

#include <QFile>

#include "util/Logging.h"

namespace qiftop::backend::linuximpl {

CgroupClassifier::CgroupClassifier()
{
    m_clock.start();
}

bool CgroupClassifier::initialize()
{
    QFile self(QStringLiteral("/proc/self/cgroup"));
    if (!self.open(QIODevice::ReadOnly)) {
        qCWarning(lcVerbose) << "CgroupClassifier: cannot open /proc/self/cgroup;"
                                " container attribution disabled";
        return false;
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
    // the dead process's container badge for up to kCacheTtlMs.
    const auto stNowOpt = procsnap::pidStartTime(pid);
    if (!stNowOpt.has_value()) return {};  // pid is gone
    const quint64 stNow = *stNowOpt;

    std::lock_guard lock(m_mu);
    const qint64 now = m_clock.elapsed();
    if (auto it = m_cache.constFind(pid); it != m_cache.constEnd()) {
        if (it->startTime == stNow && now - it->ts < kCacheTtlMs) {
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
        chain = cgroup::classifyProcCgroupChain(QString::fromUtf8(data));
    }
    // If the open failed (race: pid died between starttime read and
    // here), chain stays empty and we cache that — cheap, and the
    // entry will age out in ≤2 s anyway.

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
