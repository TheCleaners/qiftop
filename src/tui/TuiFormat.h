#pragma once

// Pure-logic formatting + sorting for the nqiftop ncurses frontend. No
// ncurses, no event loop — row -> cell strings, the bandwidth scale + gauge
// fraction, per-column sort comparators, and the per-row colour role.
// Unit-tested directly (tests/test_tui_format.cpp).

#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/Connection.h"
#include "tui/TuiTheme.h"
#include "util/Units.h"

namespace qiftop::tui {

enum class View { Interfaces, Connections };

struct Column {
    QString title;
    bool    rightAlign = false; // numeric columns are right-aligned
    int     fixedWidth = 0;     // 0 = auto (natural width); >0 = exact
};

// Fixed widths for the numeric/status columns so the layout is STABLE as
// values change (otherwise the columns jitter every tick). The name/flow
// column (index 0) flexes to fill the rest.
constexpr int kRateW   = 11; // "999.9 MiB/s"
constexpr int kTotalW  = 11; // "999.99 GiB"
constexpr int kStatusW = 9;  // "loopback"

// Layout (both views): [name/flow] [RX rate] [TX rate] [RX total] [TX total]
// (+ [Status] for interfaces). The whole-row bandwidth gauge is painted as a
// background fill by Screen — it's not a column.
inline QList<Column> columnsFor(View v)
{
    QList<Column> cols = {
        {v == View::Interfaces ? QStringLiteral("Interface") : QStringLiteral("Flow"), false, 0},
        {QStringLiteral("RX rate"),  true, kRateW},
        {QStringLiteral("TX rate"),  true, kRateW},
        {QStringLiteral("RX total"), true, kTotalW},
        {QStringLiteral("TX total"), true, kTotalW},
    };
    if (v == View::Interfaces)
        cols.append({QStringLiteral("Status"), false, kStatusW});
    return cols;
}

// --- bandwidth scale --------------------------------------------------------

inline double combinedRate(const aggregate::InterfaceAggregator::Row &r)
{
    return r.rxRate + r.txRate;
}
inline double combinedRate(const aggregate::ConnectionAggregator::Row &r)
{
    return r.rxRate + r.txRate;
}

// Round `maxRate` UP to a "nice" 1/2/5 × 10^k value so the loudest row's
// full-row gauge maps to a readable scale (like iftop's top ruler).
inline double niceScale(double maxRate)
{
    if (maxRate <= 0.0)
        return 1024.0; // 1 KiB/s floor so a quiet link still has a scale
    const double e    = std::floor(std::log10(maxRate));
    const double base = std::pow(10.0, e);
    const double m    = maxRate / base;
    const double nice = (m <= 1.0) ? 1.0 : (m <= 2.0) ? 2.0 : (m <= 5.0) ? 5.0 : 10.0;
    return nice * base;
}

// Gauge fraction in [0,1]: a row's combined rate against the view scale.
inline double gaugeFraction(double value, double scale)
{
    if (scale <= 0.0)
        return 0.0;
    return std::clamp(value / scale, 0.0, 1.0);
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

// --- per-row colour role (the qiftop direction colour-coding semantics) -----

inline Role rowRoleForInterface(const aggregate::InterfaceAggregator::Row &r)
{
    if (!r.current.isUp && !r.current.isLoopback)
        return Role::Stale;
    return Role::Normal;
}

inline Role rowRoleForConnection(const aggregate::ConnectionAggregator &agg,
                                 const aggregate::ConnectionAggregator::Row &r)
{
    if (r.stale)
        return Role::Stale;
    const Connection &c = r.current;
    if (agg.isForwardedFlow(c))
        return Role::Forwarded;
    if (c.direction == Direction::Outbound)
        return Role::Outbound;
    if (c.direction == Direction::Inbound)
        return Role::Inbound;
    return Role::Normal;
}

// --- sorting ----------------------------------------------------------------
// Column layout: 0 = name/flow (text), 1 = RX rate, 2 = TX rate,
// 3 = RX total, 4 = TX total, 5 = status (interfaces only).

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
