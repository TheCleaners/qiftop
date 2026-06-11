#include "aggregate/InterfaceAggregator.h"

#include <algorithm>

namespace qiftop::aggregate {

namespace {

double byteRate(quint64 current, quint64 previous, double deltaSecs)
{
    return current >= previous
        ? static_cast<double>(current - previous) / deltaSecs
        : 0.0;
}

} // namespace

InterfaceAggregator::InterfaceAggregator(QObject *parent)
    : QObject(parent)
{
    m_elapsed.start();
}

void InterfaceAggregator::updateStats(QList<InterfaceStats> stats)
{
    const qint64 nowMs     = m_elapsed.elapsed();
    const qint64 deltaMs   = nowMs - m_lastElapsedMs;
    m_lastElapsedMs        = nowMs;
    const double deltaSecs = deltaMs > 0 ? deltaMs / 1000.0 : 1.0;

    // Sort by name for deterministic row order; a sort proxy / the TUI can
    // re-sort on top.
    std::ranges::sort(stats, {}, &InterfaceStats::name);

    // Decide whether the row set itself changed (add/remove). A full reset is
    // simplest and correct; interface churn is rare so the cost is negligible.
    const bool structureChanged = [&] {
        if (stats.size() != m_rows.size())
            return true;
        for (qsizetype i = 0; i < stats.size(); ++i) {
            if (stats[i].name != m_rows[i].current.name)
                return true;
        }
        return false;
    }();

    if (structureChanged) {
        emit aboutToReset();
        m_rows.clear();
        m_rows.reserve(stats.size());
        for (const InterfaceStats &s : stats) {
            Row row;
            row.current = s;
            if (auto it = m_prev.constFind(s.name); it != m_prev.constEnd()) {
                row.rxRate = byteRate(s.rxBytes, it->rxBytes, deltaSecs);
                row.txRate = byteRate(s.txBytes, it->txBytes, deltaSecs);
            }
            m_rows.append(std::move(row));
        }
        emit didReset();
    } else {
        for (qsizetype i = 0; i < stats.size(); ++i) {
            const InterfaceStats &s = stats[i];
            Row &row = m_rows[i];
            if (auto it = m_prev.constFind(s.name); it != m_prev.constEnd()) {
                row.rxRate = byteRate(s.rxBytes, it->rxBytes, deltaSecs);
                row.txRate = byteRate(s.txBytes, it->txBytes, deltaSecs);
            }
            row.current = s;
        }
        if (!m_rows.isEmpty())
            emit rowsChanged(0, static_cast<int>(m_rows.size()) - 1);
    }

    // Refresh previous-sample cache.
    m_prev.clear();
    m_prev.reserve(stats.size());
    for (const InterfaceStats &s : stats)
        m_prev.insert(s.name, s);
}

} // namespace qiftop::aggregate
