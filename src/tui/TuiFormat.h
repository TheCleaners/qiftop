#pragma once

// Pure-logic formatting + sorting for the nqiftop ncurses frontend. No
// ncurses, no event loop — just row -> cell strings and the per-column sort
// comparators, so it can be unit-tested directly (tests/test_tui_format.cpp).

#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <numeric>

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/Connection.h"
#include "util/Units.h"

namespace qiftop::tui {

enum class View { Interfaces, Connections };

struct Column {
    QString title;
    bool    rightAlign = false; // numeric columns are right-aligned
};

inline QList<Column> columnsFor(View v)
{
    if (v == View::Interfaces)
        return {{QStringLiteral("Interface"), false},
                {QStringLiteral("RX rate"),   true},
                {QStringLiteral("TX rate"),   true},
                {QStringLiteral("RX total"),  true},
                {QStringLiteral("TX total"),  true},
                {QStringLiteral("Status"),    false}};
    return {{QStringLiteral("Flow"),     false},
            {QStringLiteral("RX rate"),  true},
            {QStringLiteral("TX rate"),  true},
            {QStringLiteral("RX total"), true},
            {QStringLiteral("TX total"), true}};
}

// --- cell rendering ---------------------------------------------------------

inline QStringList cellsForInterface(const aggregate::InterfaceAggregator::Row &r)
{
    const InterfaceStats &s = r.current;
    const QString status = s.isLoopback ? QStringLiteral("loopback")
                                        : (s.isUp ? QStringLiteral("up")
                                                  : QStringLiteral("down"));
    return {s.name,
            util::formatByteRate(r.rxRate),
            util::formatByteRate(r.txRate),
            util::formatBytes(s.rxBytes),
            util::formatBytes(s.txBytes),
            status};
}

// Needs the aggregator for protoLabel/endpointText (DNS- and family-aware).
inline QStringList cellsForConnection(const aggregate::ConnectionAggregator &agg,
                                      const aggregate::ConnectionAggregator::Row &r)
{
    const Connection &c = r.current;
    const QString flow = QStringLiteral("%1  %2 \u2192 %3")
                             .arg(agg.protoLabel(c),
                                  agg.endpointText(c.local),
                                  agg.endpointText(c.remote));
    return {flow,
            util::formatByteRate(r.rxRate),
            util::formatByteRate(r.txRate),
            util::formatBytes(c.rxBytes),
            util::formatBytes(c.txBytes)};
}

// --- sorting (returns a view-index vector; the aggregator rows are untouched) ---
//
// Columns 1..4 are numeric (rates / totals); column 0 is the text key
// (interface name / remote address); the interface Status column (5) sorts by
// up-ness. A stable ascending sort then an optional reverse keeps equal rows
// in their existing (key) order so the view doesn't jitter.

inline QList<int> sortedInterfaceIndices(const QList<aggregate::InterfaceAggregator::Row> &rows,
                                         int col, bool descending)
{
    QList<int> idx(rows.size());
    std::iota(idx.begin(), idx.end(), 0);
    const auto num = [&](int i) -> double {
        const auto &r = rows[i];
        switch (col) {
        case 1:  return r.rxRate;
        case 2:  return r.txRate;
        case 3:  return static_cast<double>(r.current.rxBytes);
        case 4:  return static_cast<double>(r.current.txBytes);
        case 5:  return r.current.isUp ? 1.0 : 0.0;
        default: return 0.0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (col == 0)
            return rows[a].current.name < rows[b].current.name;
        return num(a) < num(b);
    });
    if (descending)
        std::reverse(idx.begin(), idx.end());
    return idx;
}

inline QList<int> sortedConnectionIndices(const QList<aggregate::ConnectionAggregator::Row> &rows,
                                          int col, bool descending)
{
    QList<int> idx(rows.size());
    std::iota(idx.begin(), idx.end(), 0);
    const auto num = [&](int i) -> double {
        const auto &r = rows[i];
        switch (col) {
        case 1:  return r.rxRate;
        case 2:  return r.txRate;
        case 3:  return static_cast<double>(r.current.rxBytes);
        case 4:  return static_cast<double>(r.current.txBytes);
        default: return 0.0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (col == 0)
            return rows[a].current.remote.address.toString()
                 < rows[b].current.remote.address.toString();
        return num(a) < num(b);
    });
    if (descending)
        std::reverse(idx.begin(), idx.end());
    return idx;
}

} // namespace qiftop::tui
