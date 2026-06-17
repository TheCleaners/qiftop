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

// Stable column identity. Once columns can be hidden (Process / Container), a
// bare visual index is no longer a safe primary key — the sort field and the
// persisted preference both reference a ColumnId so they survive a column
// disappearing. Flow and Interface are both "column 0"; they're distinct ids
// so the sort comparator can do the right thing (remote-address vs name).
enum class ColumnId {
    Flow, RxRate, TxRate, RxTotal, TxTotal,
    Process, Container,
    Interface, Mtu, State, ErrDrop,
};

struct Column {
    ColumnId id        = ColumnId::Flow;
    QString  title;
    bool     rightAlign = false; // numeric columns are right-aligned
    int      fixedWidth = 0;     // 0 = auto (natural width); >0 = exact
    bool     hideable   = false; // optional column the user can show/hide
    bool     available  = true;  // capability allows it (Fields overlay state)
    bool     visible    = true;  // user pref shows it (Fields overlay state)
};

// Which optional Connections columns to include. Callers pass values that are
// ALREADY AND-gated on capability + user preference; columnsFor() only adds
// the grouped-redundancy rule on top.
struct OptionalColumns {
    bool process   = false;
    bool container = false;
};

// Group flows by interface / process / container (the GUI's
// ConnectionGroupProxy modes), computed TUI-side over the aggregator rows.
// Declared up here because columnsFor() consults it for the grouped-redundancy
// rule (hide Process when grouped By Process, etc.).
enum class GroupBy { None, Interface, Process, Container, Count };

// Fixed widths for the numeric/status columns so the layout is STABLE as
// values change (otherwise the columns jitter every tick). The name/flow
// column (index 0) flexes to fill the rest.
constexpr int kRateW   = 11; // "999.9 MiB/s"
constexpr int kTotalW  = 11; // "999.99 GiB"
constexpr int kStateW  = 9;  // "lower-dn" / "dormant"
constexpr int kMtuW    = 6;  // "65535"
constexpr int kErrW    = 11; // "err/drop"
constexpr int kIfaceNameW = 16; // pad iface name so the detail suffix aligns
constexpr int kProcessW   = 18; // "chromium [12345]"
constexpr int kContainerW = 22; // "containerd:abcdef123456"

// Connections layout: [flow] [RX rate] [TX rate] [RX total] [TX total], then
// optionally [Process] and/or [Container] when the agent advertises the
// matching attribution and the column isn't made redundant by the grouping.
// Interfaces layout (richer, mirrors the Qt UI's detail info):
//   [iface+addrs] [RX rate] [TX rate] [RX total] [TX total] [MTU] [State] [Err/Drop]
// The whole-row bandwidth gauge is painted as a background fill by Screen.
inline QList<Column> columnsFor(View v, OptionalColumns optional = {},
                                GroupBy groupBy = GroupBy::None)
{
    if (v == View::Interfaces) {
        // Interfaces is fixed at 8 columns; optional/groupBy don't apply.
        return {
            {ColumnId::Interface, QStringLiteral("Interface"), false, 0},
            {ColumnId::RxRate,    QStringLiteral("RX rate"),   true, kRateW},
            {ColumnId::TxRate,    QStringLiteral("TX rate"),   true, kRateW},
            {ColumnId::RxTotal,   QStringLiteral("RX total"),  true, kTotalW},
            {ColumnId::TxTotal,   QStringLiteral("TX total"),  true, kTotalW},
            {ColumnId::Mtu,       QStringLiteral("MTU"),       true,  kMtuW},
            {ColumnId::State,     QStringLiteral("State"),     false, kStateW},
            {ColumnId::ErrDrop,   QStringLiteral("Err/Drop"),  true,  kErrW},
        };
    }
    QList<Column> cols = {
        {ColumnId::Flow,    QStringLiteral("Flow"),     false, 0},
        {ColumnId::RxRate,  QStringLiteral("RX rate"),  true, kRateW},
        {ColumnId::TxRate,  QStringLiteral("TX rate"),  true, kRateW},
        {ColumnId::RxTotal, QStringLiteral("RX total"), true, kTotalW},
        {ColumnId::TxTotal, QStringLiteral("TX total"), true, kTotalW},
    };
    // Optional attribution columns. The grouped-by column is redundant with the
    // group header, so it's dropped while grouping by it (GUI parity).
    if (optional.process && groupBy != GroupBy::Process)
        cols.append({ColumnId::Process, QStringLiteral("Process"),
                     false, kProcessW, /*hideable*/true});
    if (optional.container && groupBy != GroupBy::Container)
        cols.append({ColumnId::Container, QStringLiteral("Container"),
                     false, kContainerW, /*hideable*/true});
    return cols;
}

// Every selectable field for the Fields overlay (Connections always includes
// Process + Container even when hidden/unavailable; Interfaces is its fixed 8).
// The caller annotates available/visible from caps + preference.
inline QList<Column> overlayColumnsFor(View v)
{
    if (v == View::Interfaces)
        return columnsFor(v);
    return columnsFor(v, OptionalColumns{true, true}, GroupBy::None);
}

// Stable persistence token for a sort field (so the saved preference survives a
// column being added/removed/reordered, unlike the old positional integer).
inline QString columnIdToken(ColumnId id)
{
    switch (id) {
    case ColumnId::Flow:      return QStringLiteral("flow");
    case ColumnId::RxRate:    return QStringLiteral("rxRate");
    case ColumnId::TxRate:    return QStringLiteral("txRate");
    case ColumnId::RxTotal:   return QStringLiteral("rxTotal");
    case ColumnId::TxTotal:   return QStringLiteral("txTotal");
    case ColumnId::Process:   return QStringLiteral("process");
    case ColumnId::Container: return QStringLiteral("container");
    case ColumnId::Interface: return QStringLiteral("interface");
    case ColumnId::Mtu:       return QStringLiteral("mtu");
    case ColumnId::State:     return QStringLiteral("state");
    case ColumnId::ErrDrop:   return QStringLiteral("errDrop");
    }
    return QStringLiteral("flow");
}

inline ColumnId columnIdFromToken(const QString &token, ColumnId fallback)
{
    static const struct { const char *t; ColumnId id; } kMap[] = {
        {"flow", ColumnId::Flow}, {"rxRate", ColumnId::RxRate},
        {"txRate", ColumnId::TxRate}, {"rxTotal", ColumnId::RxTotal},
        {"txTotal", ColumnId::TxTotal}, {"process", ColumnId::Process},
        {"container", ColumnId::Container}, {"interface", ColumnId::Interface},
        {"mtu", ColumnId::Mtu}, {"state", ColumnId::State},
        {"errDrop", ColumnId::ErrDrop},
    };
    for (const auto &e : kMap)
        if (token == QLatin1String(e.t))
            return e.id;
    return fallback;
}

// Map a pre-0.3.1 persisted positional sort index to a stable ColumnId, so an
// upgrading user keeps their saved sort. Old layouts:
//   Connections: 0 flow, 1 RX rate, 2 TX rate, 3 RX total, 4 TX total.
//   Interfaces:  0 iface, 1 RX rate, 2 TX rate, 3 RX total, 4 TX total,
//                5 MTU, 6 State, 7 Err/Drop.
inline ColumnId columnIdForLegacyIndex(View v, int idx)
{
    if (v == View::Interfaces) {
        switch (idx) {
        case 0:  return ColumnId::Interface;
        case 1:  return ColumnId::RxRate;
        case 2:  return ColumnId::TxRate;
        case 3:  return ColumnId::RxTotal;
        case 4:  return ColumnId::TxTotal;
        case 5:  return ColumnId::Mtu;
        case 6:  return ColumnId::State;
        case 7:  return ColumnId::ErrDrop;
        default: return ColumnId::Interface;
        }
    }
    switch (idx) {
    case 0:  return ColumnId::Flow;
    case 1:  return ColumnId::RxRate;
    case 2:  return ColumnId::TxRate;
    case 3:  return ColumnId::RxTotal;
    case 4:  return ColumnId::TxTotal;
    default: return ColumnId::RxRate;
    }
}

// Visual position of `field` in `cols`, or -1 when it isn't currently shown
// (a hidden optional column that's still the sort field — Screen then omits the
// header arrow, while the Fields overlay still marks it).
inline int visualIndexForColumn(const QList<Column> &cols, ColumnId field)
{
    for (int i = 0; i < cols.size(); ++i)
        if (cols[i].id == field)
            return i;
    return -1;
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

// Process column text: `comm [pid]` when attributed (or `pid N` when comm is
// empty), else a compact reason label so a forwarded/orphaned flow reads as
// intentional rather than a bug (GUI parity, ConnectionModel::Column::Process).
inline QString processCellText(const Connection &c)
{
    if (c.process.valid()) {
        const QString name = c.process.comm.isEmpty()
                                 ? QStringLiteral("pid %1").arg(c.process.pid)
                                 : c.process.comm;
        return QStringLiteral("%1 [%2]").arg(name).arg(c.process.pid);
    }
    switch (c.reason) {
    case AttributionReason::Forwarded:     return QStringLiteral("\u2014 forwarded \u2014");
    case AttributionReason::Orphaned:      return QStringLiteral("\u2014 orphaned \u2014");
    case AttributionReason::NoLocalSocket: return QStringLiteral("\u2014 no socket \u2014");
    case AttributionReason::Resolved:      break; // pid==0 + Resolved: unset reason
    }
    return QStringLiteral("\u2014");
}

// Container column text: `runtime:name` (name falling back to the 12-char id
// prefix), `(host)` when the flow has no container scope, with a ▸ hint when a
// nested ancestry exists (full chain stays in Detail).
inline QString containerCellText(const Connection &c)
{
    const auto &ci = c.container;
    if (ci.runtime.isEmpty() && ci.name.isEmpty() && ci.id.isEmpty())
        return QStringLiteral("(host)");
    const QString display = !ci.name.isEmpty() ? ci.name : ci.id.left(12);
    QString primary = ci.runtime.isEmpty()
                          ? display
                          : QStringLiteral("%1:%2").arg(ci.runtime, display);
    if (c.containerChain.size() >= 2)
        return QStringLiteral("%1 \u25b8").arg(primary);
    return primary;
}

inline QString connectionFlowText(const aggregate::ConnectionAggregator &agg,
                                   const Connection &c)
{
    return QStringLiteral("%1  %2 \u2192 %3")
        .arg(agg.protoLabel(c), agg.endpointText(c.local), agg.endpointText(c.remote));
}

// Column-driven cell formatting: returns one cell per entry in `columns`, in
// the same order, so the row stays index-aligned with Frame::columns (which
// Screen::render() relies on). Optional Process/Container cells appear only
// when their column is present.
inline QStringList cellsForConnection(const aggregate::ConnectionAggregator &agg,
                                      const aggregate::ConnectionAggregator::Row &r,
                                      const QList<Column> &columns
                                          = columnsFor(View::Connections))
{
    const Connection &c = r.current;
    QStringList cells;
    cells.reserve(columns.size());
    for (const Column &col : columns) {
        switch (col.id) {
        case ColumnId::Flow:      cells << connectionFlowText(agg, c); break;
        case ColumnId::RxRate:    cells << util::formatByteRate(r.rxRate); break;
        case ColumnId::TxRate:    cells << util::formatByteRate(r.txRate); break;
        case ColumnId::RxTotal:   cells << util::formatBytes(c.rxBytes); break;
        case ColumnId::TxTotal:   cells << util::formatBytes(c.txBytes); break;
        case ColumnId::Process:   cells << processCellText(c); break;
        case ColumnId::Container: cells << containerCellText(c); break;
        default:                  cells << QString(); break;
        }
    }
    return cells;
}

// Group-header cells: the aggregated traffic per column, with the grouped
// label in the (always-present) first cell and empty optional cells (the
// grouped value lives in the header label / group-info window, not a column).
inline QStringList groupHeaderCells(const QList<Column> &columns,
                                    const QString &labelCell,
                                    double rxRate, double txRate,
                                    quint64 rxBytes, quint64 txBytes)
{
    QStringList cells;
    cells.reserve(columns.size());
    for (const Column &col : columns) {
        switch (col.id) {
        case ColumnId::Flow:    cells << labelCell; break;
        case ColumnId::RxRate:  cells << util::formatByteRate(rxRate); break;
        case ColumnId::TxRate:  cells << util::formatByteRate(txRate); break;
        case ColumnId::RxTotal: cells << util::formatBytes(rxBytes); break;
        case ColumnId::TxTotal: cells << util::formatBytes(txBytes); break;
        default:                cells << QString(); break;
        }
    }
    return cells;
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
// Sorting keys off a stable ColumnId, not a visual index, so a hidden optional
// column being the sort field still works (and the persisted preference is
// portable across column layouts).

// Container sort key: by name, then id, then a sentinel that sorts host/no-
// container rows to one end (GUI parity, ConnectionModel SortRole).
inline QString containerSortKey(const Connection &c)
{
    if (!c.container.name.isEmpty()) return c.container.name;
    if (!c.container.id.isEmpty())   return c.container.id;
    return QStringLiteral("\u0001(host)");
}

inline QList<int> sortedInterfaceIndices(const QList<aggregate::InterfaceAggregator::Row> &rows,
                                         ColumnId field, bool descending)
{
    QList<int> idx(rows.size());
    std::iota(idx.begin(), idx.end(), 0);
    const auto num = [&](int i) -> double {
        const auto &r = rows[i];
        switch (field) {
        case ColumnId::RxRate:  return r.rxRate;
        case ColumnId::TxRate:  return r.txRate;
        case ColumnId::RxTotal: return static_cast<double>(r.current.rxBytes);
        case ColumnId::TxTotal: return static_cast<double>(r.current.txBytes);
        case ColumnId::Mtu:     return static_cast<double>(r.current.mtu);
        case ColumnId::State:   return static_cast<double>(r.current.operState);
        case ColumnId::ErrDrop: return static_cast<double>(r.current.rxErrors + r.current.txErrors
                                                           + r.current.rxDropped + r.current.txDropped);
        default:                return 0.0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (field == ColumnId::Interface)
            return rows[a].current.name < rows[b].current.name;
        return num(a) < num(b);
    });
    if (descending)
        std::reverse(idx.begin(), idx.end());
    return idx;
}

inline QList<int> sortedConnectionIndices(const QList<aggregate::ConnectionAggregator::Row> &rows,
                                          ColumnId field, bool descending)
{
    QList<int> idx(rows.size());
    std::iota(idx.begin(), idx.end(), 0);
    const auto num = [&](int i) -> double {
        const auto &r = rows[i];
        switch (field) {
        case ColumnId::RxRate:  return r.rxRate;
        case ColumnId::TxRate:  return r.txRate;
        case ColumnId::RxTotal: return static_cast<double>(r.current.rxBytes);
        case ColumnId::TxTotal: return static_cast<double>(r.current.txBytes);
        case ColumnId::Process: return static_cast<double>(r.current.process.pid);
        default:                return 0.0;
        }
    };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        if (field == ColumnId::Flow)
            return rows[a].current.remote.address.toString()
                 < rows[b].current.remote.address.toString();
        if (field == ColumnId::Container)
            return containerSortKey(rows[a].current) < containerSortKey(rows[b].current);
        return num(a) < num(b);
    });
    if (descending)
        std::reverse(idx.begin(), idx.end());
    return idx;
}

// --- grouping (Connections view) --------------------------------------------
// GroupBy is declared near the top (columnsFor consults it); the helpers below
// operate on it.

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

// Per-group summary used to decide group display order in grouped views.
struct GroupSummary {
    double  rxRate  = 0.0;
    double  txRate  = 0.0;
    quint64 rxBytes = 0;
    quint64 txBytes = 0;
    int     minSrc  = 0;     // stable first-appearance position (source index)
    QString label;
};

// Decide the display order of groups (returns indices into `g`).
//   sortWithinGroups == true  → group order FROZEN at first-appearance
//        (minSrc ascending); re-sorting the rows never shuffles the groupings.
//   sortWithinGroups == false → classic: order groups by the sort field's
//        aggregate (RxRate/TxRate/RxTotal/TxTotal); the nonnumeric fields
//        (Flow / Process / Container) order by the group label — honouring
//        `sortDesc`, with minSrc as a stable tiebreak. Mirrors the GUI
//        ConnectionGroupProxy behaviour.
inline QList<int> orderedGroupIndices(const QList<GroupSummary> &g,
                                      ColumnId sortField, bool sortDesc,
                                      bool sortWithinGroups)
{
    QList<int> order(g.size());
    std::iota(order.begin(), order.end(), 0);
    if (sortWithinGroups) {
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return g[a].minSrc < g[b].minSrc; });
        return order;
    }
    const auto aggVal = [&](int i) -> double {
        switch (sortField) {
        case ColumnId::RxRate:  return g[i].rxRate;
        case ColumnId::TxRate:  return g[i].txRate;
        case ColumnId::RxTotal: return double(g[i].rxBytes);
        case ColumnId::TxTotal: return double(g[i].txBytes);
        default:                return 0.0;   // label-ordered fields below
        }
    };
    // Flow / Process / Container have no numeric aggregate → order by label.
    const bool byLabel = (sortField == ColumnId::Flow
                          || sortField == ColumnId::Process
                          || sortField == ColumnId::Container);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (byLabel) {
            const int c = g[a].label.compare(g[b].label, Qt::CaseInsensitive);
            if (c != 0) return sortDesc ? c > 0 : c < 0;
            return g[a].minSrc < g[b].minSrc;
        }
        const double va = aggVal(a), vb = aggVal(b);
        if (va == vb) return g[a].minSrc < g[b].minSrc;   // stable tiebreak
        return sortDesc ? va > vb : va < vb;
    });
    return order;
}

// --- settings model ---------------------------------------------------------

// onOff() is the common bool formatter. The settings list itself is built
// imperatively in TuiApp (each row carries value()/adjust() closures) so
// adding a preference is a one-liner there, not a switch case here.

inline QString onOff(bool v)
{
    return v ? QStringLiteral("on") : QStringLiteral("off");
}

// Build the Fields overlay list (top's `f` screen, now sort + show/hide). One
// row per column; `cols` carries the per-column hideable/available/visible
// state the caller annotated from caps + preference. The active sort field is
// marked with its direction (even when its column is hidden).
//   - mandatory column     → "fixed"  (+ "  ▼ desc" when it's the sort field)
//   - visible optional      → "[x]"   (+ marker)
//   - hidden optional        → "[ ]"   (+ marker)
//   - unavailable optional   → "unavailable" (cap missing; not toggleable)
inline QList<SettingRow> fieldRows(const QList<Column> &cols, ColumnId sortField,
                                   bool sortDesc)
{
    QList<SettingRow> rows;
    rows.reserve(cols.size());
    for (const Column &c : cols) {
        const bool isSort = (c.id == sortField);
        const QString marker = isSort
            ? (sortDesc ? QStringLiteral("  \u25bc desc") : QStringLiteral("  \u25b2 asc"))
            : QString();
        QString value;
        QString help;
        if (!c.hideable) {
            value = QStringLiteral("fixed") + marker;
            help  = QStringLiteral("Sort by %1. Enter sorts; r reverses.").arg(c.title);
        } else if (!c.available) {
            value = QStringLiteral("unavailable");
            const QString token = (c.id == ColumnId::Container)
                ? QStringLiteral("container-attribution-wire")
                : QStringLiteral("process-attribution-wire");
            help  = QStringLiteral("Requires the agent's %1 capability.").arg(token);
        } else {
            value = (c.visible ? QStringLiteral("[x]") : QStringLiteral("[ ]")) + marker;
            help  = QStringLiteral("Space shows/hides %1; Enter sorts; r reverses.")
                        .arg(c.title);
        }
        rows.append({c.title, value, help});
    }
    return rows;
}

} // namespace qiftop::tui
