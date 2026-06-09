#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QStringList>

#include <mutex>

#include "backend/ProcessResolver.h"

namespace qiftop::backend::linuximpl {

// PID → container/cgroup-scope classifier. Implements only the container
// half of the ProcessResolver interface; resolveFlow always returns
// nullopt (that's SockDiagResolver's job). Composed with SockDiagResolver
// behind a CompositeResolver in the factory.
//
// DATA SOURCE
//   /proc/<pid>/cgroup. Parsed and pattern-matched in
//   `qiftop::backend::cgroup::classifyProcCgroup` (header-only, in
//   CgroupParse.h) so the classification regexes are unit-testable
//   without spinning up real containers.
//
// CACHING
//   Per-PID result cache with a 2s TTL — cgroup membership only changes
//   on migration (rare; live-migrate between cgroups is admin action,
//   not normal app behaviour) and process exit. Bounded to 8 k entries
//   to keep memory predictable on hosts with thousands of short-lived
//   workloads.
class CgroupClassifier final : public ProcessResolver {
public:
    CgroupClassifier();
    ~CgroupClassifier() override = default;

    // /proc readability probe. Returns true on any normal Linux host;
    // false (with a log line) if /proc/self/cgroup can't be read at all.
    bool initialize() override;

    [[nodiscard]] QStringList capabilities() const override;

    [[nodiscard]] std::optional<ProcessInfo>
        resolveFlow(const Connection &) override { return std::nullopt; }

    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32 pid) override;

private:
    bool                              m_ready = false;
    std::mutex                        m_mu;
    QElapsedTimer                     m_clock;
    struct CacheEntry {
        std::optional<ContainerInfo> info;
        qint64                       ts;
        quint64                      startTime;  // pid identity guard
    };
    QHash<qint32, CacheEntry>         m_cache;
    static constexpr int  kCacheTtlMs    = 2000;
    static constexpr int  kCacheMaxItems = 8192;
};

} // namespace qiftop::backend::linuximpl
