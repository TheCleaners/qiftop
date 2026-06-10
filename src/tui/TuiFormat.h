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
constexpr int kStateW  = 9;  // "lower-dn" / "dormant"
constexpr int kMtuW    = 6;  // "65535"
constexpr int kErrW    = 11; // "err/drop"
constexpr int kIfaceNameW = 16; // pad iface name so the detail suffix aligns

// Connections layout: [flow] [RX rate] [TX rate] [RX total] [TX total].
// Interfaces layout (richer, mirrors the Qt UI's detail info):
//   [iface+addrs] [RX rate] [TX rate] [RX total] [TX total] [MTU] [State] [Err/Drop]
// The whole-row bandwidth gauge is painted as a background fill by Screen.
inline QList<Column> columnsFor(View v)
{
    QList<Column> cols = {
        {v == View::Interfaces ? QStringLiteral("Interface") : QStringLiteral("Flow"), false, 0},
        {QStringLiteral("RX rate"),  true, kRateW},
        {QStringLiteral("TX rate"),  true, kRateW},
        {QStringLiteral("RX total"), true, kTotalW},
        {QStringLiteral("TX total"), true, kTotalW},
    };
    if (v == View::Interfaces) {
        cols.append({QStringLiteral("MTU"),      true,  kMtuW});
        cols.append({QStringLiteral("State"),    false, kStateW});
        cols.append({QStringLiteral("Err/Drop"), true,  kErrW});
    }
    return cols;
}

// Linux IF_OPER_* (RFC 2863) -> short label, with an isUp fallback when the
// backend couldn't determine the operational state (operState == 0).
inline QString operStateText(quint8 operState, bool isUp)
{
    switch (operState) {
    case 1: return QStringLiteral("absent");
    case 2: return QStringLiteral("down");
    case 3: return QStringLiteral("lower-dn");
    case 4: return QStringLiteral("testing");
    case 5: return QStringLiteral("dormant");
    case 6: return QStringLiteral("up");
    default: return isUp ? QStringLiteral("up") : QStringLiteral("down");
    }
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

    // Flexible first column: name + the extra detail the Qt UI shows (type and
    // assigned addresses). The name is padded to a fixed sub-width so the detail
    // suffix lines up down the column.
    QString name = s.name;
    QStringList detail;
    if (s.isLoopback)
        detail << QStringLiteral("loopback");
    else if (!s.type.isEmpty())
        detail << s.type;
    if (!s.addresses.isEmpty())
        detail << s.addresses.join(QStringLiteral(", "));
    if (!detail.isEmpty())
        name = s.name.leftJustified(kIfaceNameW) + QStringLiteral("\u2014 ")
               + detail.join(QStringLiteral("  "));

    const quint64 errs  = s.rxErrors + s.txErrors;
    const quint64 drops = s.rxDropped + s.txDropped;

    return {name,
            util::formatByteRate(r.rxRate),
            util::formatByteRate(r.txRate),
            util::formatBytes(s.rxBytes),
            util::formatBytes(s.txBytes),
            s.mtu > 0 ? QString::number(s.mtu) : QStringLiteral("—"),
            operStateText(s.operState, s.isUp),
            QStringLiteral("%1/%2").arg(errs).arg(drops)};
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
    const InterfaceStats &s = r.current;
    // Loopback and down/inactive links are de-emphasised (dim); a link that is
    // up AND currently moving data is highlighted so the busy NIC stands out;
    // up-but-idle is normal. (operState 2/1 = down/absent.)
    if (s.isLoopback)
        return Role::Stale;
    if (!s.isUp || s.operState == 2 || s.operState == 1)
        return Role::Stale;
    if (r.rxRate + r.txRate > 0.0)
        return Role::Outbound;   // live link — green-ish accent
    return Role::Normal;         // up but idle
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
// Connections columns: 0 flow, 1 RX rate, 2 TX rate, 3 RX total, 4 TX total.
// Interfaces columns:  0 iface, 1 RX rate, 2 TX rate, 3 RX total, 4 TX total,
//                      5 MTU, 6 State (operState), 7 Err/Drop.

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
        case 5:  return static_cast<double>(r.current.mtu);
        case 6:  return static_cast<double>(r.current.operState);
        case 7:  return static_cast<double>(r.current.rxErrors + r.current.txErrors
                                            + r.current.rxDropped + r.current.txDropped);
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

// --- grouping (Connections view) --------------------------------------------
// Group flows by interface / process / container (the GUI's ConnectionGroupProxy
// modes), computed TUI-side over the aggregator rows.

enum class GroupBy { None, Interface, Process, Container, Count };

inline QString groupByName(GroupBy g)
{
    switch (g) {
    case GroupBy::Interface: return QStringLiteral("interface");
    case GroupBy::Process:   return QStringLiteral("process");
    case GroupBy::Container: return QStringLiteral("container");
    case GroupBy::None:      return QStringLiteral("off");
    case GroupBy::Count:     break;
    }
    return QStringLiteral("off");
}

// Stable bucket key. Empty string is a valid key (the "unattributed" bucket).
inline QString groupKeyFor(GroupBy g, const Connection &c)
{
    switch (g) {
    case GroupBy::Interface:
        return c.iface;
    case GroupBy::Process:
        return c.process.valid()
                   ? QStringLiteral("%1\u0001%2").arg(c.process.pid).arg(c.process.comm)
                   : QString();
    case GroupBy::Container:
        // Include runtime so the same id under docker vs podman never collapses.
        return c.container.valid()
                   ? QStringLiteral("%1\u0001%2").arg(c.container.runtime, c.container.id)
                   : QString();
    case GroupBy::None:
    case GroupBy::Count:
        break;
    }
    return QString();
}

inline QString groupLabelFor(GroupBy g, const Connection &c)
{
    switch (g) {
    case GroupBy::Interface:
        return c.iface.isEmpty() ? QStringLiteral("(unattributed)") : c.iface;
    case GroupBy::Process:
        if (!c.process.valid())
            return QStringLiteral("(unattributed)");
        return QStringLiteral("%1 [%2]")
            .arg(c.process.comm.isEmpty() ? QStringLiteral("?") : c.process.comm)
            .arg(c.process.pid);
    case GroupBy::Container:
        if (!c.container.valid())
            return QStringLiteral("(no container)");
        return QStringLiteral("%1 (%2)")
            .arg(c.container.name.isEmpty() ? c.container.id : c.container.name,
                 c.container.runtime);
    case GroupBy::None:
    case GroupBy::Count:
        break;
    }
    return QString();
}

// --- settings model ---------------------------------------------------------
// SettingRow is the rendering DTO for a modal list entry (label + value +
// per-row help), shared by Screen and the declarative settings model in
// TuiApp. onOff() is the common bool formatter. The settings list itself is
// built imperatively in TuiApp (each row carries value()/adjust() closures) so
// adding a preference is a one-liner there, not a switch case here.

struct SettingRow {
    QString label;
    QString value;  // human value: theme name, "on"/"off", "1000 ms", …
    QString help;   // aptitude-style one-line description
};

inline QString onOff(bool v)
{
    return v ? QStringLiteral("on") : QStringLiteral("off");
}

// Build the Fields/sort-selector list (top's `f` screen): one row per column,
// the active sort column marked with its direction. selected index maps onto a
// column index in `cols`.
inline QList<SettingRow> sortFieldRows(const QList<Column> &cols, int sortCol,
                                       bool sortDesc)
{
    QList<SettingRow> rows;
    for (int c = 0; c < cols.size(); ++c) {
        const QString value = (c == sortCol)
            ? (sortDesc ? QStringLiteral("\u25bc desc") : QStringLiteral("\u25b2 asc"))
            : QString();
        rows.append({cols[c].title, value,
                     QStringLiteral("Sort by %1. Enter selects; r/←/→ reverses.")
                         .arg(cols[c].title)});
    }
    return rows;
}

} // namespace qiftop::tui
