#pragma once

#include <algorithm>
#include <vector>

#include <QHash>
#include <QList>
#include <QLoggingCategory>

#include "backend/DeepAttribution.h"

namespace qiftop::backend {

// Bounded, deduplicating priority queue for deep-attribution requests.
//
// Discipline (per v0.4 §5.5):
//   * Dedup by AttributionFlowKey — a flow re-observed in a newer snapshot
//     updates the stored flow + generation rather than piling up duplicates.
//     The `attempts` counter is carried forward (max) so aging survives
//     re-enqueue from either the service (fresh snapshot) or the worker
//     (unresolved retry).
//   * Priority = top talkers: dequeue returns the highest total-bytes flows
//     first, preserving the service's top-by-bytes intent.
//   * Hard capacity: when full, the lowest-priority (quietest) entries are
//     dropped and a single qWarning is logged for the process lifetime.
//
// Pure: no Qt Widgets, no I/O, no platform headers — unit-tested directly.
class DeepAttributionQueue {
public:
    explicit DeepAttributionQueue(int capacity = 8192) : m_cap(capacity) {}

    void setCapacity(int c) { m_cap = c > 0 ? c : 0; enforceCap(); }
    [[nodiscard]] int capacity() const { return m_cap; }

    // Merge `reqs` in. Returns the number of requests dropped to respect the
    // cap (0 in the common case).
    int enqueue(const QList<DeepAttributionRequest> &reqs)
    {
        for (const DeepAttributionRequest &r : reqs) {
            auto it = m_byKey.find(r.key);
            if (it == m_byKey.end()) {
                m_byKey.insert(r.key, r);
            } else {
                // Keep the freshest observation but never lose accumulated
                // attempts (so a long-unresolved flow still ages out).
                const int attempts = std::max(it->attempts, r.attempts);
                DeepAttributionRequest merged = r;
                merged.generation = std::max(it->generation, r.generation);
                merged.attempts   = attempts;
                *it = merged;
            }
        }
        return enforceCap();
    }

    // Remove and return up to `batch` highest-priority (most total bytes)
    // requests. Returns fewer when the queue is smaller.
    QList<DeepAttributionRequest> dequeue(int batch)
    {
        QList<DeepAttributionRequest> out;
        if (batch <= 0 || m_byKey.isEmpty())
            return out;

        std::vector<DeepAttributionRequest> all;
        all.reserve(static_cast<std::size_t>(m_byKey.size()));
        for (const auto &r : std::as_const(m_byKey))
            all.push_back(r);

        const auto n = std::min<std::size_t>(static_cast<std::size_t>(batch),
                                             all.size());
        std::partial_sort(all.begin(), all.begin() + static_cast<long>(n),
                          all.end(), moreBytes);
        out.reserve(static_cast<int>(n));
        for (std::size_t i = 0; i < n; ++i) {
            out.append(all[i]);
            m_byKey.remove(all[i].key);
        }
        return out;
    }

    [[nodiscard]] int  size() const { return static_cast<int>(m_byKey.size()); }
    [[nodiscard]] bool isEmpty() const { return m_byKey.isEmpty(); }
    void clear() { m_byKey.clear(); }

private:
    static quint64 totalBytes(const DeepAttributionRequest &r)
    {
        return r.flow.rxBytes + r.flow.txBytes;
    }
    static bool moreBytes(const DeepAttributionRequest &a,
                          const DeepAttributionRequest &b)
    {
        return totalBytes(a) > totalBytes(b);
    }

    // Drop quietest entries until within capacity. Returns count dropped.
    int enforceCap()
    {
        if (m_cap <= 0) {
            const int dropped = m_byKey.size();
            if (dropped > 0) { m_byKey.clear(); warnOnce(dropped); }
            return dropped;
        }
        if (m_byKey.size() <= m_cap)
            return 0;

        std::vector<DeepAttributionRequest> all;
        all.reserve(static_cast<std::size_t>(m_byKey.size()));
        for (const auto &r : std::as_const(m_byKey))
            all.push_back(r);
        // Keep the loudest m_cap; drop the rest.
        std::partial_sort(all.begin(), all.begin() + m_cap, all.end(), moreBytes);
        const int dropped = static_cast<int>(all.size()) - m_cap;
        for (std::size_t i = static_cast<std::size_t>(m_cap); i < all.size(); ++i)
            m_byKey.remove(all[i].key);
        warnOnce(dropped);
        return dropped;
    }

    void warnOnce(int dropped)
    {
        if (dropped <= 0 || m_warnedOverflow)
            return;
        m_warnedOverflow = true;
        qWarning().noquote()
            << "DeepAttributionQueue: capacity" << m_cap
            << "exceeded; dropping quietest deep-attribution requests"
            << "(further drops silently suppressed)";
    }

    QHash<AttributionFlowKey, DeepAttributionRequest> m_byKey;
    int  m_cap;
    bool m_warnedOverflow = false;
};

} // namespace qiftop::backend
