#include "CgroupClassifier.h"
#include "CgroupParse.h"

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
    return { QStringLiteral("container-attribution") };
}

std::optional<ContainerInfo>
CgroupClassifier::resolveContainerForPid(qint32 pid)
{
    if (!m_ready || pid <= 0) return std::nullopt;

    std::lock_guard lock(m_mu);
    const qint64 now = m_clock.elapsed();
    if (auto it = m_cache.constFind(pid); it != m_cache.constEnd()) {
        if (now - it->ts < kCacheTtlMs) return it->info;
    }

    QFile f(QStringLiteral("/proc/%1/cgroup").arg(pid));
    std::optional<ContainerInfo> info;
    if (f.open(QIODevice::ReadOnly)) {
        // /proc/<pid>/cgroup is typically <512 bytes (v2 unified) and
        // bounded to a few KiB even on busy v1 hosts.
        const QByteArray data = f.read(4096);
        info = cgroup::classifyProcCgroup(QString::fromUtf8(data));
    }

    // Crude bound: when full, just clear and start over rather than
    // implement an LRU we don't need at this scale.
    if (m_cache.size() >= kCacheMaxItems) m_cache.clear();
    m_cache.insert(pid, {info, now});
    return info;
}

} // namespace qiftop::backend::linuximpl
