#pragma once

#include <chrono>
#include <functional>
#include <mutex>

#include "backend/BirthCache.h"
#include "backend/ProcessResolver.h"

// Birth-event process resolver (v0.4 eBPF birth+conntrack hybrid).
//
// Sits FIRST in the resolver chain. For a conntrack flow it has a cached birth
// record for, it returns the authoritative pid captured at connect()/accept()
// time — recovering short-lived processes that sock_diag misses because they've
// already exited by the 1 s dump. Flows with no birth record fall through to
// the next resolver (sock_diag), which still wins for long-lived / listening
// sockets the birth probes never saw (e.g. sockets that predate the agent).
//
// This class is the TRANSPORT-NEUTRAL core: it owns the BirthCache and the
// resolve logic, but NOT the eBPF program. The Linux loader (backend/linux)
// feeds births via onBirth() from its ring-buffer reader thread; tests inject
// births the same way with no kernel. PID-reuse validation re-checks the live
// starttime via an injectable probe (the real one reads /proc/<pid>/stat;
// tests supply a fake), so this header stays free of platform code.

namespace qiftop::backend {

class BpfBirthResolver : public ProcessResolver {
public:
    // Default monotonic-ms clock (CLOCK_MONOTONIC-equivalent), used unless a
    // test injects a fake clock for deterministic TTL aging.
    static std::function<qint64()> makeDefaultClock()
    {
        return [] {
            return static_cast<qint64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        };
    }

    // startTimeProbe(pid) returns the pid's CURRENT /proc/<pid>/stat field-22
    // starttime, or 0 if the pid is gone/unreadable. Used to reject a recycled
    // PID before serving a cached birth. When null, validation is skipped
    // (cache trusted as-is) — acceptable only in tests; the production wiring
    // always supplies it.
    explicit BpfBirthResolver(std::function<quint64(qint32)> startTimeProbe = {},
                              std::function<qint64()> clockMs = {})
        : m_startTimeProbe(std::move(startTimeProbe))
        , m_clockMs(clockMs ? std::move(clockMs) : makeDefaultClock())
    {
    }

    // Feed a captured birth (called by the ring-buffer reader thread, or a
    // test). Thread-safe: serialised with resolvePid via the same mutex.
    void onBirth(const BirthKey &key, const BirthRecord &rec)
    {
        std::scoped_lock lock(m_mu);
        const qint64 now = m_clockMs();
        m_cache.insert(key, rec, now);
        // Remember comm/uid per pid so enrichPid can answer without a /proc
        // read (we captured them at birth). Bounded alongside the cache.
        if (rec.valid()) {
            ProcessInfo pi;
            pi.pid  = rec.pid;
            pi.comm = rec.comm;
            pi.uid  = rec.uid;
            m_enrichByPid.insert(rec.pid, pi);
            if (m_enrichByPid.size() > m_cache.maxEntries())
                m_enrichByPid.clear(); // clear-on-overflow, mirrors the cache
        }
    }

    // Mark the resolver active (the eBPF program loaded + probes attached).
    // Until set, capabilities() is empty and it contributes nothing, so an
    // unsupported kernel / absent libbpf yields a clean conntrack-only chain.
    void setLoaded(bool loaded) { m_loaded = loaded; }
    [[nodiscard]] bool loaded() const { return m_loaded; }

    // --- ProcessResolver -------------------------------------------------
    bool initialize() override { return m_loaded; }

    [[nodiscard]] QStringList capabilities() const override
    {
        if (!m_loaded)
            return {};
        return {QStringLiteral("process-attribution"),
                QStringLiteral("birth-attribution")};
    }

    [[nodiscard]] qint32 resolvePid(const Connection &flow) override
    {
        if (!m_loaded)
            return 0;
        std::optional<BirthRecord> rec;
        {
            std::scoped_lock lock(m_mu);
            rec = m_cache.find(flow, m_clockMs());
        }
        if (!rec || !rec->valid())
            return 0;

        // PID-reuse guard (AGENTS.md §8a rule 2): the kernel may have recycled
        // the pid since birth. Re-check the live starttime; only reject when a
        // DIFFERENT live process now holds the pid (live != 0 && live != ours).
        //
        // A GONE pid (live == 0) must STILL be served: short-lived processes —
        // the whole reason birth attribution exists — have usually exited by
        // the time the conntrack flow is resolved, so /proc/<pid> is gone. The
        // captured (pid, comm) is the historically-correct owner of that flow,
        // and no live process is masquerading as that pid, so serving it is
        // both correct and the entire point. (If the pid were reused, a new
        // flow from the reusing process emits its own birth keyed by its tuple,
        // overwriting this entry — it never silently steals this attribution.)
        if (m_startTimeProbe) {
            const quint64 live = m_startTimeProbe(rec->pid);
            if (live != 0 && live != rec->startTime) {
                std::scoped_lock lock(m_mu);
                m_cache.remove(birthKeyOf(flow));
                return 0;
            }
        }
        return rec->pid;
    }

    // We captured comm + uid at birth; surface them without a /proc read. exe /
    // cmdline / cwd stay lazy (a later resolver or GetProcessDetails fills
    // them) — birth's value is the PID itself, before the process exits.
    [[nodiscard]] std::optional<ProcessInfo> enrichPid(qint32 pid) override
    {
        if (!m_loaded || pid <= 0)
            return std::nullopt;
        std::scoped_lock lock(m_mu);
        if (auto it = m_enrichByPid.constFind(pid); it != m_enrichByPid.constEnd())
            return it.value();
        return std::nullopt;
    }

    // Birth carries no container scope; the chain's CgroupClassifier handles
    // that on the resolved pid. Return nullopt so the composite falls through.
    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32) override { return std::nullopt; }

    // Prune expired births (called periodically by the reader thread).
    void prune()
    {
        std::scoped_lock lock(m_mu);
        m_cache.prune(m_clockMs());
    }

    [[nodiscard]] int cacheSize()
    {
        std::scoped_lock lock(m_mu);
        return m_cache.size();
    }

private:
    mutable std::mutex                 m_mu;
    BirthCache                         m_cache;
    QHash<qint32, ProcessInfo>         m_enrichByPid; // pid → comm/uid from birth
    std::function<quint64(qint32)>     m_startTimeProbe;
    std::function<qint64()>            m_clockMs;
    bool                               m_loaded = false;
};

} // namespace qiftop::backend
