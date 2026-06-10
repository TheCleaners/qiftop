#pragma once

// Pure-logic formatting + sorting for the nqiftop ncurses frontend. No
// ncurses, no event loop — row -> cell strings, the bandwidth-bar gauge, the
// dynamic scale, the per-column sort comparators, and the per-row colour
// role. Unit-tested directly (tests/test_tui_format.cpp).

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

// Fixed width of the bandwidth-bar column (iftop's signature gauge).
constexpr int kBarWidth   = 14;
// Column index of the bandwidth bar in both views.
constexpr int kBarColumn  = 1;

struct Column {
    QString title;
    bool    rightAlign = false; // numeric columns are right-aligned
    int     fixedWidth = 0;     // 0 = auto (natural width); >0 = exact
};

// Columns. Layout (both views): [name/flow] [bar] [RX rate] [TX rate]
// [RX total] [TX total] (+ [Status] for interfaces). The bar column's title
// is overwritten with the live scale by the caller.
inline QList<Column> columnsFor(View v)
{
    QList<Column> cols = {
        {v == View::Interfaces ? QStringLiteral("Interface") : QStringLiteral("Flow"), false, 0},
        {QStringLiteral("Bandwidth"), false, kBarWidth},
        {QStringLiteral("RX rate"),   true,  0},
        {QStringLiteral("TX rate"),   true,  0},
        {QStringLiteral("RX total"),  true,  0},
        {QStringLiteral("TX total"),  true,  0},
    };
    if (v == View::Interfaces)
        cols.append({QStringLiteral("Status"), false, 0});
    return cols;
}

// --- bandwidth scale + bar --------------------------------------------------

inline double combinedRate(const aggregate::InterfaceAggregator::Row &r)
{
    return r.rxRate + r.txRate;
}
inline double combinedRate(const aggregate::ConnectionAggregator::Row &r)
{
    return r.rxRate + r.txRate;
}

// Round `maxRate` UP to a "nice" 1/2/5 × 10^k value so a full bar maps to a
// readable scale (like iftop's top ruler). Returns >= maxRate.
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

// A `width`-cell horizontal bar: filled block (█) proportional to value/scale.
inline QString barString(double value, double scale, int width)
{
    if (width <= 0)
        return {};
    double frac = scale > 0.0 ? value / scale : 0.0;
    frac = std::clamp(frac, 0.0, 1.0);
    int filled = static_cast<int>(std::lround(frac * width));
    filled = std::clamp(filled, 0, width);
    // A single visible cell for any non-zero traffic, so tiny flows still show.
    if (filled == 0 && value > 0.0)
        filled = 1;
    return QString(filled, QChar(0x2588)) + QString(width - filled, QLatin1Char(' '));
}

// --- cell rendering (the bar cell is filled in by the caller, which knows the
//     view-wide scale; here it's an empty placeholder) -----------------------

inline QStringList cellsForInterface(const aggregate::InterfaceAggregator::Row &r)
{
    const InterfaceStats &s = r.current;
    const QString status = s.isLoopback ? QStringLiteral("loopback")
                                        : (s.isUp ? QStringLiteral("up")
                                                  : QStringLiteral("down"));
    return {s.name,
            QString(),                       // bar placeholder
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
            QString(),                       // bar placeholder
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
// Column layout: 0 = name/flow (text), 1 = bandwidth (combined rate),
// 2 = RX rate, 3 = TX rate, 4 = RX total, 5 = TX total, 6 = status (iface).

inline QList<int> sortedInterfaceIndices(const QList<aggregate::InterfaceAggregator::Row> &rows,
                                         int col, bool descending)
{
    QList<int> idx(rows.size());
    std::iota(idx.begin(), idx.end(), 0);
    const auto num = [&](int i) -> double {
        const auto &r = rows[i];
        switch (col) {
        case 1:  return combinedRate(r);
        case 2:  return r.rxRate;
        case 3:  return r.txRate;
        case 4:  return static_cast<double>(r.current.rxBytes);
        case 5:  return static_cast<double>(r.current.txBytes);
        case 6:  return r.current.isUp ? 1.0 : 0.0;
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
        case 1:  return combinedRate(r);
        case 2:  return r.rxRate;
        case 3:  return r.txRate;
        case 4:  return static_cast<double>(r.current.rxBytes);
        case 5:  return static_cast<double>(r.current.txBytes);
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
