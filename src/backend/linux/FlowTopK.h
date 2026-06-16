#pragma once

#include "backend/Connection.h"

#include <QList>

#include <algorithm>

// Pure, header-only bounded top-K-by-bytes collector used by the in-process
// ConntrackMonitor to cap each emitted snapshot at the loudest N flows
// (mirroring the agent's ConnectionsService cap). Extracted here so the
// admission/eviction logic is unit-testable without a live conntrack handle
// (tests/test_flow_topk.cpp); the monitor's nfct callback calls admitFlowTopK
// per flow.
namespace qiftop::backend::linux {

[[nodiscard]] inline quint64 flowBytesTotal(const Connection &c)
{
    return c.rxBytes + c.txBytes;
}

// Comparator that makes std::*_heap a MIN-heap by total bytes: the heap front
// (`heap.first()`) is the smallest-byte flow, i.e. the eviction candidate.
struct FlowMinHeapByBytes {
    bool operator()(const Connection &a, const Connection &b) const
    {
        return flowBytesTotal(a) > flowBytesTotal(b);
    }
};

// Offer one flow to a bounded top-K min-heap.
//   * cap <= 0  → unbounded: just append (no heap maintained).
//   * heap not full → append + sift up.
//   * heap full and the flow is louder than the current smallest → evict the
//     smallest and insert this one.
// `heap` must be maintained as a min-heap (only via this function) so its
// front is always the smallest-byte element. After a full dump it holds the
// `cap` loudest flows in heap (not sorted) order — callers that need display
// order sort downstream, exactly as with the agent's capped snapshot.
inline void admitFlowTopK(QList<Connection> &heap, const Connection &c, int cap)
{
    if (cap <= 0) {
        heap.append(c);
        return;
    }
    if (heap.size() < cap) {
        heap.append(c);
        std::push_heap(heap.begin(), heap.end(), FlowMinHeapByBytes{});
    } else if (flowBytesTotal(c) > flowBytesTotal(heap.constFirst())) {
        std::pop_heap(heap.begin(), heap.end(), FlowMinHeapByBytes{}); // min → back
        heap.back() = c;
        std::push_heap(heap.begin(), heap.end(), FlowMinHeapByBytes{});
    }
}

} // namespace qiftop::backend::linux
