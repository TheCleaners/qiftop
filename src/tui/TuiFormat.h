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
#include "backend/ProcessDetails.h"
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

// --- stable row identity (for the expand/collapse set, survives re-sort) -----

inline QString interfaceKey(const aggregate::InterfaceAggregator::Row &r)
{
    return r.current.name;
}
inline QString connectionKey(const aggregate::ConnectionAggregator::Row &r)
{
    return r.current.key();
}

// --- modal row DTO ----------------------------------------------------------
// Rendering DTO for one modal list entry (label + value + per-row help),
// shared by Screen, the per-row Detail panel, and the declarative settings
// model in TuiApp.
struct SettingRow {
    QString label;
    QString value;  // human value: theme name, "on"/"off", "1000 ms", …
    QString help;   // aptitude-style one-line description
};

// --- detail rows for the per-row detail modal (label / value pairs) ---------
// Shown in a centred panel opened with Enter — full width, never reflowing the
// live list. Empty/optional fields are still listed (with an em-dash) for the
// interface; connection process/container rows appear only when attributed.

inline QList<SettingRow> interfaceDetailRows(const aggregate::InterfaceAggregator::Row &r)
{
    const InterfaceStats &s = r.current;
    return {
        {QStringLiteral("Type"),
         s.isLoopback ? QStringLiteral("loopback")
                      : (s.type.isEmpty() ? QStringLiteral("\u2014") : s.type), {}},
        {QStringLiteral("Addresses"),
         s.addresses.isEmpty() ? QStringLiteral("\u2014")
                               : s.addresses.join(QStringLiteral(", ")), {}},
        {QStringLiteral("MTU"), s.mtu ? QString::number(s.mtu) : QStringLiteral("\u2014"), {}},
        {QStringLiteral("ifindex"), QString::number(s.ifIndex), {}},
        {QStringLiteral("State"),
         QStringLiteral("%1 (oper %2)").arg(operStateText(s.operState, s.isUp)).arg(s.operState), {}},
        {QStringLiteral("RX rate"), util::formatByteRate(r.rxRate), {}},
        {QStringLiteral("TX rate"), util::formatByteRate(r.txRate), {}},
        {QStringLiteral("RX total"), util::formatBytes(s.rxBytes), {}},
        {QStringLiteral("TX total"), util::formatBytes(s.txBytes), {}},
        {QStringLiteral("Packets"),
         QStringLiteral("rx %1  tx %2").arg(s.rxPackets).arg(s.txPackets), {}},
        {QStringLiteral("Errors"),
         QStringLiteral("rx %1  tx %2").arg(s.rxErrors).arg(s.txErrors), {}},
        {QStringLiteral("Dropped"),
         QStringLiteral("rx %1  tx %2").arg(s.rxDropped).arg(s.txDropped), {}},
    };
}

// Why an unattributed flow has no process — mirrors the GUI's synthetic
// Process-column label so the TUI detail panel explains it too.
inline QString unattributedLabel(AttributionReason reason)
{
    switch (reason) {
    case AttributionReason::Forwarded:     return QStringLiteral("(forwarded — no local process)");
    case AttributionReason::Orphaned:      return QStringLiteral("(orphaned — socket gone)");
    case AttributionReason::NoLocalSocket: return QStringLiteral("(no local socket)");
    case AttributionReason::Resolved:      break;
    }
    return QStringLiteral("(unattributed)");
}

// Shared helper: append Exe / Cmdline / Cwd rows from on-demand process
// details (the agent's GetProcessDetails RPC). No-op when pd is null or the
// fields are empty, so the panel only grows once the async reply lands.
inline void appendProcessDetailRows(QList<SettingRow> &rows,
                                    const qiftop::backend::ProcessDetails *pd)
{
    if (!pd || !pd->valid())
        return;
    if (!pd->exe.isEmpty())
        rows << SettingRow{QStringLiteral("Exe"), pd->exe, {}};
    if (!pd->cmdline.isEmpty())
        rows << SettingRow{QStringLiteral("Cmdline"), pd->cmdline, {}};
    if (!pd->cwd.isEmpty())
        rows << SettingRow{QStringLiteral("Cwd"), pd->cwd, {}};
}

inline QList<SettingRow> connectionDetailRows(const aggregate::ConnectionAggregator &agg,
                                              const aggregate::ConnectionAggregator::Row &r,
                                              const qiftop::backend::ProcessDetails *pd = nullptr)
{
    const Connection &c = r.current;
    QList<SettingRow> rows;
    rows << SettingRow{QStringLiteral("Protocol"), agg.protoLabel(c), {}};
    rows << SettingRow{QStringLiteral("Local"),  agg.endpointText(c.local), {}};
    rows << SettingRow{QStringLiteral("Remote"), agg.endpointText(c.remote), {}};
    const QString dir = c.direction == Direction::Outbound ? QStringLiteral("outbound")
                      : c.direction == Direction::Inbound  ? QStringLiteral("inbound")
                                                           : QStringLiteral("unknown");
    rows << SettingRow{QStringLiteral("Direction"), dir, {}};
    rows << SettingRow{QStringLiteral("Interface"),
                       QStringLiteral("%1 (ifindex %2)")
                           .arg(c.iface.isEmpty() ? QStringLiteral("\u2014") : c.iface)
                           .arg(c.ifIndex), {}};
    const QString tcp = tcpStateToString(c.tcpState);
    if (!tcp.isEmpty())
        rows << SettingRow{QStringLiteral("TCP state"), tcp, {}};
    // Process + container attribution — shown whenever present, in every view
    // mode (flat included), not only when grouped.
    rows << SettingRow{QStringLiteral("Process"),
                       c.process.valid()
                           ? QStringLiteral("%1 [%2] uid %3")
                                 .arg(c.process.comm.isEmpty() ? QStringLiteral("?") : c.process.comm)
                                 .arg(c.process.pid).arg(c.process.uid)
                           : unattributedLabel(c.reason), {}};
    appendProcessDetailRows(rows, pd);   // exe/cmdline/cwd once fetched
    if (c.container.valid())
        rows << SettingRow{QStringLiteral("Container"),
                           QStringLiteral("%1 %2 (%3)")
                               .arg(c.container.runtime,
                                    c.container.name.isEmpty() ? c.container.id : c.container.name,
                                    c.container.id), {}};
    rows << SettingRow{QStringLiteral("RX rate"), util::formatByteRate(r.rxRate), {}};
    rows << SettingRow{QStringLiteral("TX rate"), util::formatByteRate(r.txRate), {}};
    rows << SettingRow{QStringLiteral("RX total"), util::formatBytes(c.rxBytes), {}};
    rows << SettingRow{QStringLiteral("TX total"), util::formatBytes(c.txBytes), {}};
    rows << SettingRow{QStringLiteral("Packets"),
                       QStringLiteral("rx %1  tx %2").arg(c.rxPackets).arg(c.txPackets), {}};
    return rows;
}

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

// Parse a group-by name (CLI / config). Returns GroupBy::Count when the token
// is unrecognised so callers can ignore an invalid override.
inline GroupBy groupByFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("off") || n == QLatin1String("none") || n == QLatin1String("flat"))
        return GroupBy::None;
    if (n == QLatin1String("interface") || n == QLatin1String("iface") || n == QLatin1String("if"))
        return GroupBy::Interface;
    if (n == QLatin1String("process") || n == QLatin1String("proc"))
        return GroupBy::Process;
    if (n == QLatin1String("container") || n == QLatin1String("ctr"))
        return GroupBy::Container;
    return GroupBy::Count; // unrecognised
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

// Group-info window (Enter on a group header): the group's shared attribution
// plus its aggregate throughput and flow count. `rep` is a representative
// member (all flows in a Process / Container group share their attribution);
// `pd` carries on-demand exe/cmdline/cwd for a Process group once fetched.
inline QList<SettingRow> groupDetailRows(GroupBy mode, const Connection &rep,
                                         double rxRate, double txRate,
                                         quint64 rxBytes, quint64 txBytes,
                                         int flowCount,
                                         const qiftop::backend::ProcessDetails *pd = nullptr)
{
    QList<SettingRow> rows;
    rows << SettingRow{QStringLiteral("Grouping"), groupByName(mode), {}};
    switch (mode) {
    case GroupBy::Interface:
        rows << SettingRow{QStringLiteral("Interface"),
                           QStringLiteral("%1 (ifindex %2)")
                               .arg(rep.iface.isEmpty() ? QStringLiteral("(unattributed)") : rep.iface)
                               .arg(rep.ifIndex), {}};
        break;
    case GroupBy::Process:
        if (rep.process.valid()) {
            rows << SettingRow{QStringLiteral("Process"),
                               rep.process.comm.isEmpty() ? QStringLiteral("?")
                                                          : rep.process.comm, {}};
            rows << SettingRow{QStringLiteral("PID"),
                               QString::number(rep.process.pid), {}};
            rows << SettingRow{QStringLiteral("UID"),
                               QString::number(rep.process.uid), {}};
            appendProcessDetailRows(rows, pd);   // exe/cmdline/cwd once fetched
        } else {
            rows << SettingRow{QStringLiteral("Process"),
                               QStringLiteral("(unattributed)"), {}};
        }
        break;
    case GroupBy::Container:
        if (rep.container.valid()) {
            rows << SettingRow{QStringLiteral("Runtime"), rep.container.runtime, {}};
            rows << SettingRow{QStringLiteral("Container"),
                               rep.container.name.isEmpty() ? rep.container.id
                                                            : rep.container.name, {}};
            rows << SettingRow{QStringLiteral("Id"), rep.container.id, {}};
        } else {
            rows << SettingRow{QStringLiteral("Container"),
                               QStringLiteral("(no container)"), {}};
        }
        break;
    case GroupBy::None:
    case GroupBy::Count:
        break;
    }
    // Container scope is interesting on a Process group too.
    if (mode == GroupBy::Process && rep.container.valid())
        rows << SettingRow{QStringLiteral("Container"),
                           QStringLiteral("%1 %2")
                               .arg(rep.container.runtime,
                                    rep.container.name.isEmpty() ? rep.container.id
                                                                 : rep.container.name), {}};
    rows << SettingRow{QStringLiteral("Flows"), QString::number(flowCount), {}};
    rows << SettingRow{QStringLiteral("RX rate"), util::formatByteRate(rxRate), {}};
    rows << SettingRow{QStringLiteral("TX rate"), util::formatByteRate(txRate), {}};
    rows << SettingRow{QStringLiteral("RX total"), util::formatBytes(rxBytes), {}};
    rows << SettingRow{QStringLiteral("TX total"), util::formatBytes(txBytes), {}};
    return rows;
}

// Word-wrap `text` to `width` columns: break on spaces, but hard-break any
// single token longer than the width so a long path / cmdline / key-help
// description never overflows a dialog (and never garbles the column
// alignment). Returns at least one line (possibly empty). Width clamps to >= 1.
// Pure + header-only so it's unit-tested without an ncurses screen.
inline QStringList wrapToWidth(const QString &text, int width)
{
    width = std::max(1, width);
    QStringList out;
    if (text.isEmpty()) { out << QString(); return out; }
    QString line;
    const QStringList words = text.split(QLatin1Char(' '));
    const auto flush = [&] { out << line; line.clear(); };
    for (const QString &w : words) {
        QString word = w;
        while (word.size() > width) {          // hard-break an over-long token
            if (!line.isEmpty()) flush();
            out << word.left(width);
            word = word.mid(width);
        }
        if (line.isEmpty())
            line = word;
        else if (line.size() + 1 + word.size() <= width)
            line += QLatin1Char(' ') + word;
        else { flush(); line = word; }
    }
    flush();
    if (out.isEmpty()) out << QString();
    return out;
}

// --- settings model ---------------------------------------------------------
// onOff() is the common bool formatter. The settings list itself is built
// imperatively in TuiApp (each row carries value()/adjust() closures) so
// adding a preference is a one-liner there, not a switch case here.

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
