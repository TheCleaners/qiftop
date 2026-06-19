#pragma once

#include <optional>

#include <QHash>
#include <QHostAddress>
#include <QString>
#include <QtGlobal>

#include "backend/Connection.h"

// Birth-event attribution cache (v0.4 eBPF birth+conntrack hybrid).
//
// An eBPF program records (pid, comm, uid, direction, starttime, first-seen) at
// flow BIRTH — the instant connect()/accept()/first UDP send fires, in the
// owning process's context, BEFORE a short-lived process can exit. A userspace
// reader inserts those into this cache; the BpfBirthResolver looks each
// conntrack flow up here FIRST, recovering the pid that sock_diag would miss
// because the process is already gone by the 1 s dump.
//
// Widgets-free, header-only, no platform headers, no Qt event loop — pure data
// structure, unit-tested without loading any BPF (events are injected). The
// real eBPF loader lives in backend/linux and feeds insert().

namespace qiftop::backend {

// Direction-AGNOSTIC 5-tuple key. Deliberately NOT AttributionFlowKey: the
// birth event and the conntrack flow may infer/observe direction differently
// (birth knows it definitionally from connect-vs-accept; conntrack's is a
// heuristic), and ifIndex may differ between the socket's bound device and the
// flow's egress device. Correlating on {proto, local, remote} alone is what
// reliably matches a birth to its conntrack flow.
struct BirthKey {
    L4Proto      proto = L4Proto::Unknown;
    QHostAddress localAddress;
    quint16      localPort = 0;
    QHostAddress remoteAddress;
    quint16      remotePort = 0;

    friend bool operator==(const BirthKey &, const BirthKey &) = default;
};

[[nodiscard]] inline BirthKey birthKeyOf(const Connection &c)
{
    return BirthKey{
        .proto         = c.proto,
        .localAddress  = c.local.address,
        .localPort     = c.local.port,
        .remoteAddress = c.remote.address,
        .remotePort    = c.remote.port,
    };
}

inline size_t qHash(const BirthKey &k, size_t seed = 0) noexcept
{
    return qHashMulti(seed,
                      static_cast<quint8>(k.proto),
                      k.localAddress,
                      k.localPort,
                      k.remoteAddress,
                      k.remotePort);
}

// One captured birth. starttime is the owning process's /proc/<pid>/stat field
// 22 (jiffies since boot), snapshotted at insert so a later lookup can reject a
// recycled PID (AGENTS.md §8a rule 2).
struct BirthRecord {
    qint32    pid = 0;
    quint32   uid = 0;
    QString   comm;        // kernel comm (≤15 bytes), captured at birth
    Direction direction = Direction::Unknown;
    quint64   startTime = 0;          // jiffies since boot
    qint64    firstSeenMonoMs = 0;    // CLOCK_MONOTONIC ms at birth
    qint64    insertedMonoMs = 0;     // when we cached it (for TTL aging)

    [[nodiscard]] bool valid() const { return pid > 0; }
};

// Bounded, TTL'd birth cache. Single-writer (the ring-buffer reader thread) /
// reader (the data thread) in production; callers serialise access with their
// own mutex (the cache itself is not internally locked — keep it a plain data
// structure so it stays trivially testable). All times are CLOCK_MONOTONIC ms.
class BirthCache {
public:
    static constexpr int   kDefaultMaxEntries = 65536; // hard cap (§8a rule 8)
    static constexpr qint64 kDefaultTtlMs     = 30'000; // a birth never matched ages out

    explicit BirthCache(int maxEntries = kDefaultMaxEntries,
                        qint64 ttlMs = kDefaultTtlMs)
        : m_maxEntries(maxEntries > 0 ? maxEntries : kDefaultMaxEntries)
        , m_ttlMs(ttlMs > 0 ? ttlMs : kDefaultTtlMs)
    {
    }

    // Insert/replace a birth. `nowMs` is the current CLOCK_MONOTONIC ms (passed
    // in so tests drive aging deterministically). On overflow the whole cache
    // is cleared (clear-on-overflow per §8a rule 8 — at this scale LRU isn't
    // worth it; the hot set repopulates from the next births). Returns true if
    // an overflow-clear happened.
    bool insert(const BirthKey &key, const BirthRecord &rec, qint64 nowMs)
    {
        bool cleared = false;
        if (!m_map.contains(key) && m_map.size() >= m_maxEntries) {
            m_map.clear();
            cleared = true;
        }
        BirthRecord r = rec;
        r.insertedMonoMs = nowMs;
        m_map.insert(key, r);
        return cleared;
    }

    // Look up a birth for a flow. Returns the record only if present AND not
    // expired. PID-reuse validation (re-checking the live starttime) is the
    // CALLER's job — the cache has no /proc access; the resolver does it.
    [[nodiscard]] std::optional<BirthRecord> find(const BirthKey &key,
                                                  qint64 nowMs) const
    {
        auto it = m_map.constFind(key);
        if (it == m_map.constEnd())
            return std::nullopt;
        if (nowMs - it->insertedMonoMs > m_ttlMs)
            return std::nullopt; // stale; prune() will reap it
        return *it;
    }

    [[nodiscard]] std::optional<BirthRecord> find(const Connection &flow,
                                                  qint64 nowMs) const
    {
        return find(birthKeyOf(flow), nowMs);
    }

    // Drop a consumed/stale birth (e.g. once a flow it matched has closed).
    void remove(const BirthKey &key) { m_map.remove(key); }

    // Reap expired entries. Cheap to call periodically from the reader thread.
    // Returns the number pruned.
    int prune(qint64 nowMs)
    {
        int n = 0;
        for (auto it = m_map.begin(); it != m_map.end();) {
            if (nowMs - it->insertedMonoMs > m_ttlMs) {
                it = m_map.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
        return n;
    }

    [[nodiscard]] int  size() const { return static_cast<int>(m_map.size()); }
    [[nodiscard]] bool isEmpty() const { return m_map.isEmpty(); }
    void clear() { m_map.clear(); }

    [[nodiscard]] int    maxEntries() const { return m_maxEntries; }
    [[nodiscard]] qint64 ttlMs() const { return m_ttlMs; }

private:
    QHash<BirthKey, BirthRecord> m_map;
    int    m_maxEntries;
    qint64 m_ttlMs;
};

} // namespace qiftop::backend
