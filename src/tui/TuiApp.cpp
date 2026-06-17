#include "tui/TuiApp.h"

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"
#include "backend/PlatformInfo.h"
#include "util/Exportable.h"
#include "util/Exporter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSysInfo>
#include <QtGlobal>
#include <QTimer>

#include <algorithm>
#include <limits>

// ncurses last (KEY_* constants); after Qt to avoid macro clashes.
#include "tui/Curses.h"

namespace qiftop::tui {

namespace {
constexpr int kRedrawThrottleMs = 33; // ~30 fps cap
constexpr int kFlashMs          = 4000; // transient status visible duration
constexpr auto kRepoUrl = "https://github.com/TheCleaners/qiftop";

QString endpointText(const Endpoint &e)
{
    const QString host = e.address.toString();
    return e.isIPv6() ? QStringLiteral("[%1]:%2").arg(host).arg(e.port)
                      : QStringLiteral("%1:%2").arg(host).arg(e.port);
}

// Exportable adapter over a snapshot of connection aggregator rows. Numeric
// columns return real numbers so the CSV is analysable; util::exporter::toCsv
// guards against spreadsheet formula injection in text fields.
class ConnRowsExportable : public Exportable {
public:
    explicit ConnRowsExportable(QList<aggregate::ConnectionAggregator::Row> rows)
        : m_rows(std::move(rows)) {}
    QStringList exportHeaders() const override
    {
        return {QStringLiteral("proto"), QStringLiteral("local"),
                QStringLiteral("remote"), QStringLiteral("iface"),
                QStringLiteral("rx_rate"), QStringLiteral("tx_rate"),
                QStringLiteral("rx_bytes"), QStringLiteral("tx_bytes"),
                QStringLiteral("comm"), QStringLiteral("pid"),
                QStringLiteral("uid"), QStringLiteral("container_runtime"),
                QStringLiteral("container")};
    }
    int exportRowCount() const override { return int(m_rows.size()); }
    QVariantList exportRow(int row) const override
    {
        const auto &r = m_rows.at(row);
        const Connection &c = r.current;
        const QString container = c.container.valid()
            ? (c.container.name.isEmpty() ? c.container.id : c.container.name)
            : QString();
        return {l4ProtoToString(c.proto), endpointText(c.local), endpointText(c.remote),
                c.iface, r.rxRaw, r.txRaw,
                static_cast<qulonglong>(c.rxBytes), static_cast<qulonglong>(c.txBytes),
                c.process.valid() ? c.process.comm : QString(),
                c.process.valid() ? c.process.pid : 0,
                c.process.valid() ? static_cast<qint64>(c.process.uid) : 0,
                c.container.runtime, container};
    }
private:
    QList<aggregate::ConnectionAggregator::Row> m_rows;
};

// Exportable adapter over a snapshot of interface aggregator rows.
class IfaceRowsExportable : public Exportable {
public:
    explicit IfaceRowsExportable(QList<aggregate::InterfaceAggregator::Row> rows)
        : m_rows(std::move(rows)) {}
    QStringList exportHeaders() const override
    {
        return {QStringLiteral("iface"), QStringLiteral("up"),
                QStringLiteral("rx_rate"), QStringLiteral("tx_rate"),
                QStringLiteral("rx_bytes"), QStringLiteral("tx_bytes")};
    }
    int exportRowCount() const override { return int(m_rows.size()); }
    QVariantList exportRow(int row) const override
    {
        const auto &r = m_rows.at(row);
        return {r.current.name, r.current.isUp ? 1 : 0, r.rxRate, r.txRate,
                static_cast<qulonglong>(r.current.rxBytes),
                static_cast<qulonglong>(r.current.txBytes)};
    }
private:
    QList<aggregate::InterfaceAggregator::Row> m_rows;
};

QList<int> displayedConnectionIndices(const QList<aggregate::ConnectionAggregator::Row> &rows,
                                      const aggregate::ConnectionAggregator &agg,
                                      ColumnId sortField,
                                      bool sortDesc,
                                      const qiftop::filter::ExprPtr &filterExpr)
{
    const QList<int> order = sortedConnectionIndices(rows, sortField, sortDesc);
    QList<int> matched;
    matched.reserve(order.size());
    for (int i : order) {
        if (!filterExpr) {
            matched << i;
            continue;
        }
        const auto &row = rows[i];
        const Connection &c = row.current;
        const qiftop::filter::Context ctx{
            c, row.rxRate, row.txRate,
            agg.cachedHostname(c.local.address),
            agg.cachedHostname(c.remote.address),
        };
        if (qiftop::filter::matches(filterExpr, ctx))
            matched << i;
    }
    return matched;
}

QList<aggregate::ConnectionAggregator::Row>
rowsForIndices(const QList<aggregate::ConnectionAggregator::Row> &rows,
               const QList<int> &indices)
{
    QList<aggregate::ConnectionAggregator::Row> out;
    out.reserve(indices.size());
    for (int i : indices)
        out << rows[i];
    return out;
}

QString uniqueExportPath(const QString &baseName, const QString &stamp)
{
    QDir dir = QDir::current();
    QString name = QStringLiteral("%1-%2.csv").arg(baseName, stamp);
    if (!dir.exists(name))
        return dir.absoluteFilePath(name);

    for (int n = 1; ; ++n) {
        name = QStringLiteral("%1-%2-%3.csv").arg(baseName, stamp).arg(n);
        if (!dir.exists(name))
            return dir.absoluteFilePath(name);
    }
}
}


TuiApp::TuiApp(Screen *screen,
               aggregate::InterfaceAggregator  *ifaceAgg,
               aggregate::ConnectionAggregator *connAgg,
               QString sourceLabel,
               const QString &themeName,
               int pollMs,
               const QString &viewName,
               const QString &groupName,
               QObject *parent)
    : QObject(parent)
    , m_screen(screen)
    , m_ifaceAgg(ifaceAgg)
    , m_connAgg(connAgg)
    , m_sourceLabel(std::move(sourceLabel))
{
    m_themes = builtinThemes();

    // Restore persisted state first, then let explicit command-line options
    // (--theme, -i/--interval, --view, --group) override the saved values.
    loadSettings();
    if (!themeName.isEmpty()) {
        for (int i = 0; i < m_themes.size(); ++i) {
            if (m_themes[i].name.compare(themeName, Qt::CaseInsensitive) == 0) {
                m_themeIdx = i;
                break;
            }
        }
    }
    if (pollMs > 0)                          // explicit CLI interval wins
        m_pollMs = std::clamp(pollMs, 100, 10000);
    if (!viewName.isEmpty()) {
        const QString v = viewName.trimmed().toLower();
        if (v.startsWith(QLatin1Char('i')))
            m_view = View::Interfaces;
        else if (v.startsWith(QLatin1Char('c')))
            m_view = View::Connections;
    }
    if (!groupName.isEmpty()) {
        const GroupBy g = groupByFromName(groupName);
        if (g != GroupBy::Count)             // ignore an unrecognised token
            m_groupBy = g;
    }
    if (m_screen)
        m_screen->setTheme(m_themes[m_themeIdx]);
    applyAggregatorSettings();   // push restored DNS/UDP/smoothing toggles
    buildSettingItems();         // declarative settings model (uses restored state)

    // Persist on any exit path (q, SIGINT, SIGTERM all route through quit()).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { saveSettings(); });

    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setSingleShot(true);
    connect(m_redrawTimer, &QTimer::timeout, this, &TuiApp::doRedraw);

    // Transient footer status (export confirmations etc.), auto-cleared.
    m_flashTimer = new QTimer(this);
    m_flashTimer->setSingleShot(true);
    connect(m_flashTimer, &QTimer::timeout, this, [this] {
        m_flashMsg.clear();
        requestRedraw();
    });

    // Smoothing tick: advance the display tween toward the EMA target between
    // polls (mirrors the GUI). The aggregator emits rowsUpdated -> redraw.
    m_smoothTimer = new QTimer(this);
    m_smoothTimer->setInterval(std::max(100, m_pollMs / 4));
    connect(m_smoothTimer, &QTimer::timeout, this,
            [this] { if (m_connAgg) m_connAgg->advanceSmoothing(); });
    m_smoothTimer->start();

    // Any data change -> coalesced repaint (suppressed while paused).
    connect(m_ifaceAgg, &aggregate::InterfaceAggregator::didReset,
            this, &TuiApp::onDataChanged);
    connect(m_ifaceAgg, &aggregate::InterfaceAggregator::rowsChanged,
            this, [this](int, int) { onDataChanged(); });
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsInserted,
            this, &TuiApp::onDataChanged);
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsRemoved,
            this, &TuiApp::onDataChanged);
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsUpdated,
            this, [this](int, int) { onDataChanged(); });
    connect(m_connAgg, &aggregate::ConnectionAggregator::viewDataChanged,
            this, &TuiApp::onDataChanged);

    requestRedraw();
}

void TuiApp::requestRedraw()
{
    if (m_redrawPending)
        return;
    m_redrawPending = true;
    m_redrawTimer->start(kRedrawThrottleMs);
}

void TuiApp::doRedraw()
{
    m_redrawPending = false;
    if (m_screen)
        m_screen->render(buildFrame());
}

void TuiApp::handleKey(int key)
{
    if (m_overlay == Overlay::Settings) {
        handleSettingsKey(key);
        return;
    }
    if (m_overlay == Overlay::Fields) {
        handleFieldsKey(key);
        return;
    }
    if (m_overlay == Overlay::Help || m_overlay == Overlay::About) {
        handleInfoKey(key);
        return;
    }
    if (m_overlay == Overlay::Detail) {
        handleDetailKey(key);
        return;
    }
    if (m_filterEditing) {
        handleFilterKey(key);
        return;
    }
    if (m_exportPrompt) {
        handleExportKey(key);
        return;
    }

    bool &sortDesc = (m_view == View::Interfaces) ? m_ifaceSortDesc : m_connSortDesc;
    const int bodyH = std::max(1, m_screen ? m_screen->bodyHeight() : 1);

    switch (key) {
    case 'q':
    case 'Q':
        if (m_screen) m_screen->shutdown();
        QCoreApplication::quit();
        return;
    case 'S':
    case KEY_F(2):
        m_overlay = Overlay::Settings;
        break;
    case 'f':
    case 'F':
        // Start the Fields overlay on the current sort field's row.
        m_fieldsSel = std::max(0, visualIndexForColumn(overlayColumns(), currentSortField()));
        m_overlay = Overlay::Fields;
        break;
    case '?':
    case KEY_F(1):
        m_overlay = Overlay::Help;
        break;
    case 'a':
    case 'A':
        m_overlay = Overlay::About;
        break;
    case 'p':
    case 'P':
        m_paused = !m_paused;
        if (m_paused) {
            // Freeze the current view; the aggregators keep updating live so
            // unpausing shows fresh data.
            if (m_ifaceAgg) m_frozenIfaceRows = m_ifaceAgg->rows();
            if (m_connAgg)  m_frozenConnRows  = m_connAgg->rows();
            m_smoothTimer->stop();
        } else {
            m_frozenIfaceRows.clear();
            m_frozenConnRows.clear();
            m_smoothTimer->start();
        }
        break;
    case 'w':
        exportCurrentView();           // auto-named, timestamped file
        break;
    case 'W':
        promptExportFilename();         // prompt for a filename ("save as")
        break;
    case '/':
        m_filterEditing = true;
        m_filterDraft = m_filterText;   // edit the current filter in place
        break;
    case 27: // Esc — clear an active filter
        if (!m_filterText.isEmpty()) {
            m_filterText.clear();
            m_filterExpr.reset();
            m_connCursor = 0;
        } else {
            return; // nothing to do; no repaint
        }
        break;
    case '\t':
    case KEY_BTAB:
        m_view = (m_view == View::Interfaces) ? View::Connections : View::Interfaces;
        break;
    case '1':
        m_view = View::Interfaces;
        break;
    case '2':
        m_view = View::Connections;
        break;
    case 's':
        cycleSortField();
        break;
    case 'r':
    case 'R':
        sortDesc = !sortDesc;
        break;
    case 'z':
    case 'Z':
        if (!m_themes.isEmpty()) {
            m_themeIdx = (m_themeIdx + 1) % m_themes.size();
            if (m_screen)
                m_screen->setTheme(m_themes[m_themeIdx]);
        }
        break;
    case 'g':
        m_groupBy = static_cast<GroupBy>((static_cast<int>(m_groupBy) + 1)
                                         % static_cast<int>(GroupBy::Count));
        m_connCursor = 0;
        m_collapsedGroups.clear(); // group keys differ per mode; start fresh
        break;
    // --- current-line cursor: vim + arrow navigation ---
    case 'k':
    case KEY_UP:
        moveCursor(-1);
        break;
    case 'j':
    case KEY_DOWN:
        moveCursor(+1);
        break;
    case 'G':                       // vim: jump to bottom
    case KEY_END:
        moveCursor(std::numeric_limits<int>::max() / 2);
        break;
    case 4:                         // Ctrl-D: half page down
        moveCursor(bodyH / 2);
        break;
    case 21:                        // Ctrl-U: half page up
        moveCursor(-bodyH / 2);
        break;
    case KEY_PPAGE:
    case 2:                         // Ctrl-B: page up (vim/less pager)
        moveCursor(-bodyH);
        break;
    case KEY_NPAGE:
    case 6:                         // Ctrl-F: page down (vim/less pager)
        moveCursor(bodyH);
        break;
    case KEY_HOME:
        moveCursor(-(std::numeric_limits<int>::max() / 2));
        break;
    // --- open detail / expand-collapse a group ---
    case '\n':
    case '\r':
    case KEY_ENTER:
        // On a group header, open the group-info window; on a flow row, the
        // per-flow inspector. (Fold/unfold stays on h/l and Space.)
        if (cursorOnHeader())
            openGroupDetail();
        else
            openDetail();
        break;
    case ' ':
        // Space is the quick fold toggle on a header; the inspector on a flow.
        if (cursorOnHeader())
            toggleCollapseAtCursor();
        else
            openDetail();
        break;
    case 'l':
    case KEY_RIGHT:
        // Expand a collapsed group header; otherwise open the detail inspector.
        if (cursorOnHeader())
            expandAtCursor();
        else
            openDetail();
        break;
    case 'h':
    case KEY_LEFT:
        collapseAtCursor(); // fold the cursor's group (header or member)
        break;
    case KEY_RESIZE:
        break; // Screen reads the new size on the next render
    default:
        return; // ignore unknown keys without a repaint
    }
    requestRedraw();
}

Frame TuiApp::buildFrame()
{
    Frame f;
    f.tabs        = {QStringLiteral("Interfaces"), QStringLiteral("Connections")};
    f.activeTab   = (m_view == View::Interfaces) ? 0 : 1;
    f.columns     = activeColumns();
    // When grouped, advertise the grouping mode in the (always-present) first
    // column header instead of a bare "Flow" — the rows are bucketed by it.
    // The Column identity used for sorting is unchanged (this only re-labels
    // the displayed header text).
    if (m_view == View::Connections && m_groupBy != GroupBy::None && !f.columns.isEmpty())
        f.columns[0].title = QStringLiteral("Flow \u00b7 by %1").arg(groupByName(m_groupBy));

    QList<double> rates;       // combined rate per displayed row (for the gauge)
    double maxRate = 0.0;
    double aggRx = 0.0, aggTx = 0.0;

    m_rowRefs.clear();

    // Deferred cell formatting (PERF #12): build a lightweight PLAN of the
    // displayed rows and accumulate the cheap per-row data (rates, roles,
    // refs, gauge) for ALL of them — the gauge scale and the scrollbar need
    // the full list — but format the expensive cells for ONLY the visible
    // window once the scroll offset is known. cellsForConnection() /
    // cellsForInterface() (per-row rate/address/DNS formatting) are the hot
    // part; on a list far taller than the viewport this skips thousands of
    // format calls per redraw. Output is byte-identical to formatting every
    // row: Screen only ever reads f.rows[scrollOffset .. +bodyHeight).
    struct PlannedRow {
        int         src = -1;       // index into *iRows / *cRows; -1 = group header
        bool        indent = false; // grouped member row (leading "  ")
        QStringList groupCells;     // precomputed header cells (used when src < 0)
    };
    QList<PlannedRow> plan;
    const QList<aggregate::InterfaceAggregator::Row>*  iRows = nullptr;
    const QList<aggregate::ConnectionAggregator::Row>* cRows = nullptr;

    if (m_view == View::Interfaces) {
        f.sortCol  = visualIndexForColumn(f.columns, m_ifaceSortField);
        f.sortDesc = m_ifaceSortDesc;
        const auto &rows = m_paused ? m_frozenIfaceRows : m_ifaceAgg->rows();
        iRows = &rows;
        const QList<int> order =
            sortedInterfaceIndices(rows, m_ifaceSortField, m_ifaceSortDesc);
        for (int i : order) {
            const auto &row = rows[i];
            plan       << PlannedRow{i, false, {}};
            f.rowRoles << rowRoleForInterface(row);
            const double cr = combinedRate(row);
            rates << cr;
            maxRate = std::max(maxRate, cr);
            aggRx += row.rxRate;
            aggTx += row.txRate;
            m_rowRefs << RowRef{true, false, interfaceKey(row), QString()};
        }
    } else {
        f.sortCol  = visualIndexForColumn(f.columns, m_connSortField);
        f.sortDesc = m_connSortDesc;
        const auto &rows = m_paused ? m_frozenConnRows : m_connAgg->rows();
        cRows = &rows;
        const QList<int> matched =
            displayedConnectionIndices(rows, *m_connAgg, m_connSortField,
                                       m_connSortDesc, m_filterExpr);

        // Direction colours can be disabled (a customization point): fall back
        // to Normal, but still mark stale rows so they read as dimmed.
        const auto connRole = [this](const aggregate::ConnectionAggregator::Row &r) {
            if (m_directionColors)
                return rowRoleForConnection(*m_connAgg, r);
            return r.stale ? Role::Stale : Role::Normal;
        };

        if (m_groupBy == GroupBy::None) {
            for (int i : matched) {
                const auto &row = rows[i];
                plan       << PlannedRow{i, false, {}};
                f.rowRoles << connRole(row);
                const double cr = combinedRate(row);
                rates << cr;
                maxRate = std::max(maxRate, cr);
                aggRx += row.rxRate;
                aggTx += row.txRate;
                m_rowRefs << RowRef{true, false, connectionKey(row), QString()};
            }
        } else {
            // Bucket by group key. Members within a bucket inherit `matched`'s
            // order, so rows inside a group are always sorted by the active
            // column regardless of the group-ordering policy below.
            QList<QString> firstSeen;          // keys in matched (sorted) order
            QHash<QString, QList<int>> buckets;
            for (int i : matched) {
                const QString k = groupKeyFor(m_groupBy, rows[i].current);
                if (!buckets.contains(k))
                    firstSeen << k;
                buckets[k] << i;
            }

            // Per-group aggregates + the smallest source index (its stable
            // first-appearance position — the source list is key-sorted, so
            // this order is independent of the active row sort). Built in
            // firstSeen order; orderedGroupIndices() then picks the display
            // order per the sortWithinGroups policy.
            QList<GroupSummary> summaries;
            summaries.reserve(firstSeen.size());
            for (const QString &k : std::as_const(firstSeen)) {
                const QList<int> &members = buckets[k];
                GroupSummary gs;
                gs.minSrc = members.first();
                for (int i : members) {
                    gs.rxRate  += rows[i].rxRate;
                    gs.txRate  += rows[i].txRate;
                    gs.rxBytes += rows[i].current.rxBytes;
                    gs.txBytes += rows[i].current.txBytes;
                    gs.minSrc   = std::min(gs.minSrc, i);
                }
                gs.label = groupLabelFor(m_groupBy, rows[members.first()].current);
                summaries << gs;
            }

            const QList<int> gorder = orderedGroupIndices(
                summaries, m_connSortField, m_connSortDesc, m_sortWithinGroups);

            for (int oi : gorder) {
                const QString &k = firstSeen[oi];
                const QList<int> &members = buckets[k];
                const GroupSummary &gs = summaries[oi];
                const double grx = gs.rxRate, gtx = gs.txRate;
                const quint64 grb = gs.rxBytes, gtb = gs.txBytes;
                // Group header row: aggregated, and a landable cursor target so
                // it can be folded/unfolded. A ▸ (collapsed) / ▾ (expanded)
                // marker leads the label. Header cells are cheap aggregates, so
                // they're built here in phase A (only per-flow cells defer).
                const bool collapsed = m_collapsedGroups.contains(k);
                const QString marker = collapsed ? QStringLiteral("\u25b8")  // ▸
                                                 : QStringLiteral("\u25be"); // ▾
                const QString label =
                    QStringLiteral("%1 %2  (%3)").arg(marker, gs.label).arg(members.size());
                plan << PlannedRow{-1, false,
                                   groupHeaderCells(f.columns, label, grx, gtx, grb, gtb)};
                f.rowRoles << Role::GroupHeader;
                const double gcr = grx + gtx;
                rates << gcr;
                maxRate = std::max(maxRate, gcr);
                aggRx += grx;
                aggTx += gtx;
                m_rowRefs << RowRef{true, true, QString(), k};
                // Member rows (indented), unless the group is collapsed.
                if (collapsed)
                    continue;
                for (int i : members) {
                    const auto &row = rows[i];
                    plan       << PlannedRow{i, true, {}};
                    f.rowRoles << connRole(row);
                    const double cr = combinedRate(row);
                    rates << cr;
                    maxRate = std::max(maxRate, cr);
                    m_rowRefs << RowRef{true, false, connectionKey(row), k};
                }
            }
        }
    }

    // Row-spanning bandwidth gauge: a full-width row maps to a "nice" round
    // scale >= the loudest row (iftop's ruler). Each row gets a [0,1]
    // fraction that Screen paints as a background fill behind the text.
    const double scale = niceScale(maxRate);
    if (m_gaugeEnabled) {
        for (double cr : rates)
            f.rowGauge << gaugeFraction(cr, scale);
    }

    // Summary (top-style): aggregate throughput + the gauge scale + source.
    // Pad each rate to a fixed width so the right-anchored group doesn't jitter
    // as the significant-digit count changes (e.g. "990 B/s" -> "10.50 MiB/s").
    constexpr int kSumW = 12; // fits "999.99 MiB/s" / "1023.9 KiB/s"
    const QString rxStr = util::formatByteRate(aggRx).rightJustified(kSumW);
    const QString txStr = util::formatByteRate(aggTx).rightJustified(kSumW);
    const QString scStr = util::formatByteRate(scale).leftJustified(kSumW);
    const QString src = m_paused
        ? QStringLiteral("\u23f8 PAUSED \u00b7 %1").arg(m_sourceLabel)
        : m_sourceLabel;
    f.sourceLabel = QStringLiteral("\u03a3 %1\u2193 %2\u2191 \u00b7 \u2264%3 \u00b7 %4")
                        .arg(rxStr, txStr, scStr, src);

    // Current-line cursor: clamp to the row count, then derive the viewport
    // scroll so the cursor stays visible (scroll follows selection).
    const int body  = m_screen ? m_screen->bodyHeight() : 0;
    const int total = static_cast<int>(plan.size());
    int &cursor = (m_view == View::Interfaces) ? m_ifaceCursor : m_connCursor;
    int &scroll = (m_view == View::Interfaces) ? m_ifaceScroll : m_connScroll;
    // After a collapse, snap the cursor onto the folded group's header row so
    // the user stays anchored on it (and can immediately re-expand).
    if (m_cursorTargetValid) {
        for (int i = 0; i < m_rowRefs.size(); ++i) {
            if (m_rowRefs[i].header && m_rowRefs[i].groupKey == m_cursorTargetGroup) {
                cursor = i;
                break;
            }
        }
        m_cursorTargetValid = false;
        m_cursorTargetGroup.clear();
    }
    if (total <= 0) {
        cursor = 0;
        scroll = 0;
        f.cursor = -1;
    } else {
        cursor = std::clamp(cursor, 0, total - 1);
        // Never rest on a non-selectable row (a group header): snap to the
        // nearest selectable one so j/k and Enter always act on a real row.
        if (cursor < m_rowRefs.size() && !m_rowRefs[cursor].selectable) {
            int up = cursor, dn = cursor;
            while (dn < total && dn < m_rowRefs.size() && !m_rowRefs[dn].selectable) ++dn;
            while (up >= 0 && up < m_rowRefs.size() && !m_rowRefs[up].selectable) --up;
            cursor = (dn < total && dn < m_rowRefs.size()) ? dn
                   : (up >= 0 ? up : cursor);
        }
        if (body > 0) {
            if (cursor < scroll)
                scroll = cursor;
            else if (cursor >= scroll + body)
                scroll = cursor - body + 1;
            scroll = std::clamp(scroll, 0, std::max(0, total - body));
            if (body > 1 && scroll + body < total && cursor >= scroll + body - 1)
                scroll = std::clamp(cursor - body + 2, 0, std::max(0, total - body));
        }
        f.cursor = cursor;
    }
    f.scrollOffset = scroll;

    // PERF #12 phase B: now that the scroll window is known, format the cells
    // for ONLY the visible rows. Off-screen rows get an empty placeholder so
    // f.rows stays index-aligned with rowRoles / rowGauge / rowRefs and
    // f.rows.size() == total (Screen reads content only inside the window).
    // When there's no screen yet (body == 0) we format everything, preserving
    // the original behaviour for any non-interactive caller.
    {
        const int fmtBegin = (body > 0) ? std::max(0, scroll) : 0;
        const int fmtEnd   = (body > 0) ? std::min(total, scroll + body) : total;
        f.rows.reserve(total);
        for (int i = 0; i < total; ++i) {
            if (i < fmtBegin || i >= fmtEnd) {
                f.rows << QStringList();          // off-screen placeholder
                continue;
            }
            const PlannedRow &p = plan[i];
            if (p.src < 0) {                       // group header (precomputed)
                f.rows << p.groupCells;
                continue;
            }
            QStringList cells = (m_view == View::Interfaces)
                ? cellsForInterface((*iRows)[p.src])
                : cellsForConnection(*m_connAgg, (*cRows)[p.src], f.columns);
            if (p.indent && !cells.isEmpty())
                cells[0] = QStringLiteral("  ") + cells[0];
            f.rows << cells;
        }
    }

    if (m_filterEditing) {
        // The menu bar becomes the filter input line while editing.
        f.footer = m_filterError.isEmpty()
            ? QStringLiteral(" /%1\u2588   (Enter apply · Esc cancel)").arg(m_filterDraft)
            : QStringLiteral(" /%1\u2588   ! %2").arg(m_filterDraft, m_filterError);
    } else if (m_exportPrompt) {
        // The menu bar becomes the "save as" filename input line.
        f.footer = QStringLiteral(" save as: %1\u2588   (Enter save · Esc cancel)")
                       .arg(m_exportDraft);
    } else if (!m_flashMsg.isEmpty()) {
        // Transient status (e.g. export confirmation) takes the footer line.
        f.footer = QStringLiteral(" %1").arg(m_flashMsg);
    } else if (!m_filterText.isEmpty()) {
        // Active filter: "filter:" chip + the expression, then edit/clear keys.
        f.footerHints = {
            {QStringLiteral("filter:"), m_filterText},
            {QStringLiteral("/"),   QStringLiteral("edit")},
            {QStringLiteral("Esc"), QStringLiteral("clear")},
            {QStringLiteral("q"),   QStringLiteral("quit")},
            {QStringLiteral("?"),   QStringLiteral("help")},
        };
    } else {
        f.footerHints = {
            {QStringLiteral("q"),     QStringLiteral("quit")},
            {QStringLiteral("Tab"),   QStringLiteral("view")},
            {QStringLiteral("jk"),    QStringLiteral("move")},
            {QStringLiteral("\u21b5"), QStringLiteral("details")},
            {QStringLiteral("s"),     QStringLiteral("sort")},
            {QStringLiteral("f"),     QStringLiteral("fields")},
            {QStringLiteral("g"),     QStringLiteral("group")},
            {QStringLiteral("/"),     QStringLiteral("filter")},
            {QStringLiteral("p"),     QStringLiteral("pause")},
            {QStringLiteral("w"),     QStringLiteral("export")},
            {QStringLiteral("S"),     QStringLiteral("settings")},
            {QStringLiteral("?"),     QStringLiteral("help")},
        };
        // When grouped, advertise fold/unfold (h/l) right after the group key.
        if (m_groupBy != GroupBy::None)
            f.footerHints.insert(7, {QStringLiteral("h/l"), QStringLiteral("fold")});
    }

    buildModal(f);
    return f;
}

void TuiApp::buildModal(Frame &f) const
{
    if (m_overlay == Overlay::Detail) {
        // Live per-row inspector: re-resolve the target by its stable key each
        // frame so rates stay fresh and a vanished flow is handled gracefully.
        f.modal.visible    = true;
        f.modal.selectable = false;
        f.modal.footer     = QStringLiteral("↑↓ next/prev · any other key closes");
        // Group-info window (Enter on a group header): aggregate the group's
        // live members and show the shared attribution + on-demand details.
        if (!m_detailGroupKey.isEmpty() && m_connAgg) {
            double grx = 0.0, gtx = 0.0;
            quint64 grb = 0, gtb = 0;
            Connection rep;
            int flows = 0;
            bool found = false;
            for (const auto &r : (m_paused ? m_frozenConnRows : m_connAgg->rows())) {
                if (groupKeyFor(m_detailGroupBy, r.current) != m_detailGroupKey)
                    continue;
                if (!found) { rep = r.current; found = true; }
                grx += r.rxRate;
                gtx += r.txRate;
                grb += r.current.rxBytes;
                gtb += r.current.txBytes;
                ++flows;
            }
            if (found) {
                const qiftop::backend::ProcessDetails *pd = nullptr;
                if (m_detailGroupBy == GroupBy::Process && rep.process.valid()) {
                    auto it = m_procDetails.constFind(rep.process.pid);
                    if (it != m_procDetails.constEnd()) pd = &it.value();
                }
                f.modal.title = QStringLiteral("Group — %1")
                                    .arg(groupLabelFor(m_detailGroupBy, rep));
                f.modal.items = groupDetailRows(m_detailGroupBy, rep,
                                                grx, gtx, grb, gtb, flows, pd);
                return;
            }
            f.modal.title = QStringLiteral("Group");
            f.modal.items = { SettingRow{QStringLiteral("(no longer present)"), {}, {}} };
            return;
        }
        if (m_detailView == View::Interfaces) {
            for (const auto &r : (m_paused ? m_frozenIfaceRows : m_ifaceAgg->rows()))
                if (interfaceKey(r) == m_detailKey) {
                    f.modal.title = QStringLiteral("Interface — %1").arg(r.current.name);
                    f.modal.items = interfaceDetailRows(r);
                    return;
                }
        } else {
            for (const auto &r : (m_paused ? m_frozenConnRows : m_connAgg->rows()))
                if (connectionKey(r) == m_detailKey) {
                    const qiftop::backend::ProcessDetails *pd = nullptr;
                    if (r.current.process.valid()) {
                        auto it = m_procDetails.constFind(r.current.process.pid);
                        if (it != m_procDetails.constEnd()) pd = &it.value();
                    }
                    f.modal.title = QStringLiteral("Connection — %1").arg(m_connAgg->protoLabel(r.current));
                    f.modal.items = connectionDetailRows(*m_connAgg, r, pd);
                    return;
                }
        }
        f.modal.title = QStringLiteral("Detail");
        f.modal.items = { SettingRow{QStringLiteral("(no longer present)"), {}, {}} };
        return;
    }
    if (m_overlay == Overlay::Settings) {
        f.modal.visible  = true;
        f.modal.title    = QStringLiteral("Settings");
        f.modal.items.clear();
        for (const SettingItem &it : m_settings)
            f.modal.items << SettingRow{it.label, it.value(), it.help};
        f.modal.selected = m_settingsSel;
        f.modal.footer   = QStringLiteral("↑↓ move · ←/→/Space change · S/Esc close");
    } else if (m_overlay == Overlay::Fields) {
        f.modal.visible  = true;
        f.modal.title    = QStringLiteral("Fields");
        f.modal.items    = fieldRows(overlayColumns(), currentSortField(),
                                     m_view == View::Interfaces ? m_ifaceSortDesc
                                                                : m_connSortDesc);
        f.modal.selected = m_fieldsSel;
        f.modal.footer   = QStringLiteral(
            "↑↓ move · Space show/hide · Enter sort · r reverse · f/Esc close");
    } else if (m_overlay == Overlay::Help) {
        // key → label (Accent column), description → value (wrapped column).
        const auto row = [](const QString &key, const QString &desc) {
            return SettingRow{key, desc, QString()};
        };
        f.modal.visible    = true;
        f.modal.selectable = false;
        f.modal.title      = QStringLiteral("Help — key bindings");
        f.modal.items = {
            row(QStringLiteral("Tab / 1 / 2"), QStringLiteral("Switch view (Interfaces / Connections)")),
            row(QStringLiteral("↑↓ / j k"),    QStringLiteral("Move the current-line cursor")),
            row(QStringLiteral("Enter"),        QStringLiteral("On a flow: open the detail inspector. On a group header: open the group-info window (process/container details, aggregate)")),
            row(QStringLiteral("Space"),        QStringLiteral("On a flow: detail inspector. On a group header: fold / unfold")),
            row(QStringLiteral("PgUp/PgDn ^F/^B ^U/^D"), QStringLiteral("Page up/down (^F/^B) · half page (^U/^D)")),
            row(QStringLiteral("Home/End  G"),  QStringLiteral("Jump to top / bottom")),
            row(QStringLiteral("s"),            QStringLiteral("Cycle the sort field")),
            row(QStringLiteral("f"),            QStringLiteral("Fields: sort and show/hide the Process / Container columns. Unattributed Process cells show the forwarded / orphaned / no-socket reason.")),
            row(QStringLiteral("r"),            QStringLiteral("Reverse the sort order")),
            row(QStringLiteral("/"),            QStringLiteral("Filter connections (mini-language; Esc clears)")),
            row(QStringLiteral("g"),            QStringLiteral("Group connections by interface / process / container")),
            row(QStringLiteral("h/\u2190  l/\u2192"),  QStringLiteral("Collapse / expand the group under the cursor (when grouped)")),
            row(QStringLiteral("p"),            QStringLiteral("Pause / resume live updates")),
            row(QStringLiteral("w"),            QStringLiteral("Write/export the current view to an auto-named CSV file")),
            row(QStringLiteral("W"),            QStringLiteral("Export to a CSV file, prompting for the filename")),
            row(QStringLiteral("z"),            QStringLiteral("Cycle the colour theme")),
            row(QStringLiteral("S / F2"),       QStringLiteral("Settings (gauge, DNS, smoothing…)")),
            row(QStringLiteral("? / F1"),       QStringLiteral("This help")),
            row(QStringLiteral("a"),            QStringLiteral("About (app & system info)")),
            row(QStringLiteral("q"),            QStringLiteral("Quit")),
            SettingRow{QString(), QString(), QString()}, // spacer
            SettingRow{QStringLiteral("Online docs: ") + QString::fromLatin1(kRepoUrl),
                       QString(), QString()},
        };
        f.modal.footer = QStringLiteral("any key / Esc closes");
    } else if (m_overlay == Overlay::About) {
        const auto row = [](const QString &label, const QString &value) {
            return SettingRow{label.leftJustified(12), value, QString()};
        };
        const QString name = QCoreApplication::applicationName().isEmpty()
                                 ? QStringLiteral("nqiftop")
                                 : QCoreApplication::applicationName();
        const QString ver = QCoreApplication::applicationVersion();
        QString term = QStringLiteral("unknown");
        if (m_screen) {
            const QString mode = m_screen->color256() ? QStringLiteral("256-colour")
                                 : m_screen->hasColor() ? QStringLiteral("8-colour")
                                                        : QStringLiteral("monochrome");
            term = QStringLiteral("%1×%2, %3")
                       .arg(m_screen->cols()).arg(m_screen->rows()).arg(mode);
        }
        f.modal.visible    = true;
        f.modal.selectable = false;
        f.modal.title      = QStringLiteral("About %1").arg(name);
        f.modal.items = {
            row(QStringLiteral("Application"), QStringLiteral("%1 %2").arg(name, ver)),
            row(QStringLiteral("libqiftop"),   ver),
            row(QStringLiteral("Source"),      m_sourceLabel),
            row(QStringLiteral("Qt"),          QString::fromLatin1(qVersion())),
            row(QStringLiteral("OS"),          QSysInfo::prettyProductName()),
            row(QStringLiteral("Kernel"),      QStringLiteral("%1 %2")
                    .arg(QSysInfo::kernelType(), QSysInfo::kernelVersion())),
            row(QStringLiteral("Arch"),        QSysInfo::currentCpuArchitecture()),
            row(QStringLiteral("Host"),        QSysInfo::machineHostName()),
            row(QStringLiteral("Terminal"),    term),
            row(QStringLiteral("Project"),     QString::fromLatin1(kRepoUrl)),
        };
        f.modal.footer = QStringLiteral("any key / Esc closes · iftop-style net monitor");
    }
}

void TuiApp::onDataChanged()
{
    if (m_paused)
        return;            // frozen: ignore live updates until unpaused
    requestRedraw();
}

void TuiApp::moveCursor(int delta)
{
    int &cursor = (m_view == View::Interfaces) ? m_ifaceCursor : m_connCursor;
    const int total = m_rowRefs.size();
    if (total <= 0) { cursor = 0; requestRedraw(); return; }

    const int step = (delta >= 0) ? 1 : -1;
    int remaining = std::abs(delta);
    int pos = std::clamp(cursor, 0, total - 1);
    // Advance |delta| selectable rows, hopping over group headers (the only
    // non-selectable rows now that detail is a modal, not inline).
    while (remaining > 0) {
        int next = pos + step;
        while (next >= 0 && next < total && !m_rowRefs[next].selectable)
            next += step;                 // skip group headers
        if (next < 0 || next >= total)
            break;                        // hit an edge — stay on the last good row
        pos = next;
        --remaining;
    }
    // If we started on a non-selectable row (e.g. just toggled grouping), make
    // sure we land on a selectable one in the requested direction.
    if (!m_rowRefs[pos].selectable) {
        int p = pos;
        while (p >= 0 && p < total && !m_rowRefs[p].selectable) p += step;
        if (p < 0 || p >= total) { p = pos; while (p >= 0 && p < total && !m_rowRefs[p].selectable) p -= step; }
        if (p >= 0 && p < total) pos = p;
    }
    cursor = pos;
    requestRedraw();
}

void TuiApp::openDetail()
{
    const int cursor = (m_view == View::Interfaces) ? m_ifaceCursor : m_connCursor;
    if (cursor < 0 || cursor >= m_rowRefs.size())
        return;
    const RowRef &ref = m_rowRefs[cursor];
    if (!ref.selectable || ref.key.isEmpty())
        return;                           // group header / empty — nothing to inspect
    m_detailKey      = ref.key;
    m_detailView     = m_view;
    m_detailGroupKey.clear();             // a per-flow / interface detail
    m_detailGroupBy  = GroupBy::None;
    m_overlay        = Overlay::Detail;
    // Warm on-demand process details for the flow's pid (filled live on reply).
    if (m_view == View::Connections && m_connAgg) {
        for (const auto &r : (m_paused ? m_frozenConnRows : m_connAgg->rows()))
            if (connectionKey(r) == m_detailKey) {
                if (r.current.process.valid())
                    ensureProcessDetails(r.current.process.pid);
                break;
            }
    }
    requestRedraw();
}

void TuiApp::openGroupDetail()
{
    // Enter on a group header → a group-info overlay (aggregate + the
    // representative attribution; for a process group, on-demand exe/cmdline).
    if (m_view != View::Connections || m_groupBy == GroupBy::None)
        return;
    const int cursor = m_connCursor;
    if (cursor < 0 || cursor >= m_rowRefs.size() || !m_rowRefs[cursor].header)
        return;
    m_detailGroupKey = m_rowRefs[cursor].groupKey;
    m_detailGroupBy  = m_groupBy;
    m_detailView     = View::Connections;
    m_overlay        = Overlay::Detail;
    // For a process group, warm the representative pid's details.
    if (m_groupBy == GroupBy::Process && m_connAgg) {
        for (const auto &r : (m_paused ? m_frozenConnRows : m_connAgg->rows()))
            if (groupKeyFor(m_groupBy, r.current) == m_detailGroupKey
                && r.current.process.valid()) {
                ensureProcessDetails(r.current.process.pid);
                break;
            }
    }
    requestRedraw();
}

void TuiApp::ensureProcessDetails(qint32 pid)
{
    if (pid <= 0 || !m_requestDetails)
        return;
    if (m_procDetails.contains(pid) || m_detailsRequested.contains(pid))
        return;                           // cached or already in flight
    m_detailsRequested.insert(pid);
    m_requestDetails(pid);
}

void TuiApp::onProcessDetails(const qiftop::backend::ProcessDetails &d)
{
    if (d.pid <= 0)
        return;
    m_procDetails.insert(d.pid, d);
    m_detailsRequested.remove(d.pid);
    // Repaint if an overlay that could show these details is open.
    if (m_overlay == Overlay::Detail)
        requestRedraw();
}

void TuiApp::setProcessDetailsRequester(std::function<void(qint32)> fn)
{
    m_requestDetails = std::move(fn);
}

bool TuiApp::cursorOnHeader() const
{
    const int cursor = (m_view == View::Interfaces) ? m_ifaceCursor : m_connCursor;
    return cursor >= 0 && cursor < m_rowRefs.size() && m_rowRefs[cursor].header;
}

void TuiApp::collapseAtCursor()
{
    // Grouping only exists in the Connections view; the "(unattributed)" /
    // "(no container)" bucket has an EMPTY group key, which is still a valid
    // collapse target — so gate on the grouping mode, not on a non-empty key.
    if (m_view != View::Connections || m_groupBy == GroupBy::None)
        return;
    const int cursor = m_connCursor;
    if (cursor < 0 || cursor >= m_rowRefs.size())
        return;
    const QString gk = m_rowRefs[cursor].groupKey;
    if (m_collapsedGroups.contains(gk))
        return;                               // already collapsed
    m_collapsedGroups.insert(gk);
    m_cursorTargetGroup = gk;                  // keep cursor on the folded header
    m_cursorTargetValid = true;
    requestRedraw();
}

void TuiApp::expandAtCursor()
{
    if (m_view != View::Connections || m_groupBy == GroupBy::None)
        return;
    const int cursor = m_connCursor;
    if (cursor < 0 || cursor >= m_rowRefs.size() || !m_rowRefs[cursor].header)
        return;
    if (m_collapsedGroups.remove(m_rowRefs[cursor].groupKey)) // no-op if expanded
        requestRedraw();
}

void TuiApp::toggleCollapseAtCursor()
{
    if (m_view != View::Connections || m_groupBy == GroupBy::None)
        return;
    const int cursor = m_connCursor;
    if (cursor < 0 || cursor >= m_rowRefs.size() || !m_rowRefs[cursor].header)
        return;
    const QString gk = m_rowRefs[cursor].groupKey;
    if (m_collapsedGroups.contains(gk)) {
        m_collapsedGroups.remove(gk);
    } else {
        m_collapsedGroups.insert(gk);
        m_cursorTargetGroup = gk;
        m_cursorTargetValid = true;
    }
    requestRedraw();
}

void TuiApp::flashMessage(const QString &msg)
{
    m_flashMsg = msg;
    m_flashTimer->start(kFlashMs);
    requestRedraw();
}

void TuiApp::exportCurrentView(const QString &explicitPath)
{
    // Snapshot the active view's rows (frozen copy when paused) and serialise
    // to CSV via the shared libqiftop exporter. With no explicit path ('w') the
    // file is auto-named and timestamped in the current working directory; with
    // an explicit path ('W' prompt) the user's filename is used verbatim. A
    // transient footer message reports the result.
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    QString path, err;
    QByteArray data;
    int count = 0;
    const bool iface = (m_view == View::Interfaces);

    if (iface) {
        IfaceRowsExportable ex(m_paused ? m_frozenIfaceRows : m_ifaceAgg->rows());
        count = ex.exportRowCount();
        data  = util::exporter::toCsv(ex);
    } else {
        const auto &rows = m_paused ? m_frozenConnRows : m_connAgg->rows();
        // Grouped views render synthetic group headers; CSV exports the flat
        // filtered/sorted flow set underneath so downstream tools get records.
        ConnRowsExportable ex(rowsForIndices(
            rows, displayedConnectionIndices(rows, *m_connAgg, m_connSortField,
                                             m_connSortDesc, m_filterExpr)));
        count = ex.exportRowCount();
        data  = util::exporter::toCsv(ex);
    }

    if (explicitPath.isEmpty()) {
        path = uniqueExportPath(iface ? QStringLiteral("qiftop-interfaces")
                                      : QStringLiteral("qiftop-connections"), stamp);
    } else {
        path = explicitPath;
        // Convenience: default to a .csv extension when the user omitted one.
        if (!QFileInfo(path).fileName().contains(QLatin1Char('.')))
            path += QStringLiteral(".csv");
    }

    if (util::exporter::save(path, data, &err))
        flashMessage(QStringLiteral("Exported %1 rows to %2").arg(count).arg(path));
    else
        flashMessage(QStringLiteral("Export failed: %1").arg(err));
}

void TuiApp::promptExportFilename()
{
    // Pre-fill with the auto name so Enter alone yields the default; the user
    // can edit it. Mirrors the filter input line.
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    m_exportDraft = QStringLiteral("%1-%2.csv").arg(
        m_view == View::Interfaces ? QStringLiteral("qiftop-interfaces")
                                   : QStringLiteral("qiftop-connections"), stamp);
    m_exportPrompt = true;
    requestRedraw();
}

void TuiApp::handleExportKey(int key)
{
    switch (key) {
    case 27: // Esc — cancel
        m_exportPrompt = false;
        m_exportDraft.clear();
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        m_exportPrompt = false;
        if (!m_exportDraft.trimmed().isEmpty())
            exportCurrentView(m_exportDraft.trimmed());
        m_exportDraft.clear();
        break;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (!m_exportDraft.isEmpty())
            m_exportDraft.chop(1);
        break;
    case KEY_RESIZE:
        break;
    default:
        // Allow printable characters for a path/filename (incl. '/', '.', '-').
        if (key >= 0x20 && key < 0x7f)
            m_exportDraft.append(QChar(key));
        else
            return; // ignore, no repaint
        break;
    }
    requestRedraw();
}

void TuiApp::handleDetailKey(int key)
{
    // The detail panel is a transient inspector. j/k re-target it to the
    // previous/next row (browse details without closing); anything else closes.
    switch (key) {
    case 'j':
    case KEY_DOWN:
    case 'k':
    case KEY_UP: {
        moveCursor(key == 'j' || key == KEY_DOWN ? 1 : -1);
        const int c = (m_view == View::Interfaces) ? m_ifaceCursor : m_connCursor;
        if (c >= 0 && c < m_rowRefs.size() && m_rowRefs[c].selectable) {
            if (m_rowRefs[c].header) {
                // Landed on a group header → show the group-info window.
                m_detailGroupKey = m_rowRefs[c].groupKey;
                m_detailGroupBy  = m_groupBy;
                m_detailKey.clear();
            } else {
                // Landed on a flow → show the per-flow inspector.
                m_detailKey = m_rowRefs[c].key;
                m_detailGroupKey.clear();
                m_detailGroupBy = GroupBy::None;
            }
        }
        break;
    }
    case KEY_RESIZE:
        break;
    default:
        m_overlay = Overlay::None;        // Esc / Enter / q / l / any → close
        m_detailGroupKey.clear();
        m_detailGroupBy = GroupBy::None;
        break;
    }
    requestRedraw();
}

void TuiApp::commitFilter()
{
    auto res = qiftop::filter::parse(m_filterDraft.trimmed());
    m_filterError = res.error;
    if (!m_filterError.isEmpty())
        return;            // keep editing so the user can fix the expression
    m_filterExpr  = res.expr;
    m_filterText  = m_filterDraft.trimmed();
    m_filterEditing = false;
    m_connScroll = 0;      // result set changed; start at the top
}

void TuiApp::handleFilterKey(int key)
{
    switch (key) {
    case 27: // Esc — cancel, revert to the committed filter
        m_filterEditing = false;
        m_filterError.clear();
        m_filterDraft = m_filterText;
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        commitFilter();
        break;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (!m_filterDraft.isEmpty())
            m_filterDraft.chop(1);
        m_filterError.clear();
        break;
    case KEY_RESIZE:
        break;
    default:
        if (key >= 0x20 && key < 0x7f) {     // printable ASCII
            m_filterDraft.append(QChar(key));
            m_filterError.clear();
        } else {
            return; // ignore, no repaint
        }
        break;
    }
    requestRedraw();
}

void TuiApp::applyAggregatorSettings()
{
    if (!m_connAgg)
        return;
    m_connAgg->setUdpAggregateByPeer(m_udpAggregate);
    m_connAgg->setRateSmoothingMs(m_smoothing ? 300 : 0);
    m_connAgg->setHostnameResolutionEnabled(m_dnsEnabled);
}

void TuiApp::loadSettings()
{
    QSettings s;
    s.beginGroup(QStringLiteral("nqiftop"));
    m_view = (s.value(QStringLiteral("view"), int(m_view)).toInt() == int(View::Interfaces))
                 ? View::Interfaces : View::Connections;
    // Sort field: read the legacy positional integer keys (pre-0.3.1) for
    // back-compat, then let the new stable field tokens override (authoritative).
    {
        const int legacyIface = s.value(QStringLiteral("ifaceSortCol"), -1).toInt();
        if (legacyIface >= 0)
            m_ifaceSortField = columnIdForLegacyIndex(View::Interfaces, legacyIface);
        const int legacyConn = s.value(QStringLiteral("connSortCol"), -1).toInt();
        if (legacyConn >= 0)
            m_connSortField = columnIdForLegacyIndex(View::Connections, legacyConn);
        m_ifaceSortField = columnIdFromToken(
            s.value(QStringLiteral("ifaceSortField")).toString(), m_ifaceSortField);
        m_connSortField = columnIdFromToken(
            s.value(QStringLiteral("connSortField")).toString(), m_connSortField);
    }
    m_ifaceSortDesc = s.value(QStringLiteral("ifaceSortDesc"), m_ifaceSortDesc).toBool();
    m_connSortDesc  = s.value(QStringLiteral("connSortDesc"), m_connSortDesc).toBool();
    m_gaugeEnabled  = s.value(QStringLiteral("gauge"), m_gaugeEnabled).toBool();
    m_dnsEnabled    = s.value(QStringLiteral("dns"), m_dnsEnabled).toBool();
    m_udpAggregate  = s.value(QStringLiteral("udpAggregate"), m_udpAggregate).toBool();
    m_smoothing     = s.value(QStringLiteral("smoothing"), m_smoothing).toBool();
    m_directionColors = s.value(QStringLiteral("directionColors"), m_directionColors).toBool();
    m_sortWithinGroups = s.value(QStringLiteral("sortWithinGroups"), m_sortWithinGroups).toBool();
    m_showProcessColumn   = s.value(QStringLiteral("showProcessColumn"),   m_showProcessColumn).toBool();
    m_showContainerColumn = s.value(QStringLiteral("showContainerColumn"), m_showContainerColumn).toBool();
    m_pollMs        = std::clamp(s.value(QStringLiteral("pollMs"), m_pollMs).toInt(), 100, 10000);
    {
        const int g = s.value(QStringLiteral("groupBy"), int(m_groupBy)).toInt();
        m_groupBy = (g >= 0 && g < int(GroupBy::Count)) ? static_cast<GroupBy>(g)
                                                        : GroupBy::None;
    }
    const QString themeName = s.value(QStringLiteral("theme")).toString();
    s.endGroup();

    if (!themeName.isEmpty()) {
        for (int i = 0; i < m_themes.size(); ++i)
            if (m_themes[i].name.compare(themeName, Qt::CaseInsensitive) == 0) {
                m_themeIdx = i;
                break;
            }
    }
    // Guard the persisted sort field against a token that isn't valid for the
    // view (corrupt/cross-view config) — fall back to the per-view default.
    if (visualIndexForColumn(overlayColumnsFor(View::Interfaces), m_ifaceSortField) < 0)
        m_ifaceSortField = ColumnId::Interface;
    if (visualIndexForColumn(overlayColumnsFor(View::Connections), m_connSortField) < 0)
        m_connSortField = ColumnId::RxRate;
}

void TuiApp::saveSettings() const
{
    // Don't write into another user's ~/.config when run privileged
    // (e.g. `sudo -E nqiftop`): it would leave a root-owned nqiftop.conf
    // in the invoking user's home. Settings were still loaded at startup.
    if (qiftop::platform::settingsWriteWouldEscalate()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("nqiftop: running privileged with a config directory "
                     "owned by another user; not writing settings (would "
                     "create root-owned files in their home directory).");
        }
        return;
    }
    QSettings s;
    s.beginGroup(QStringLiteral("nqiftop"));
    s.setValue(QStringLiteral("view"), int(m_view));
    s.setValue(QStringLiteral("ifaceSortField"), columnIdToken(m_ifaceSortField));
    s.setValue(QStringLiteral("ifaceSortDesc"), m_ifaceSortDesc);
    s.setValue(QStringLiteral("connSortField"), columnIdToken(m_connSortField));
    s.setValue(QStringLiteral("connSortDesc"), m_connSortDesc);
    s.setValue(QStringLiteral("gauge"), m_gaugeEnabled);
    s.setValue(QStringLiteral("dns"), m_dnsEnabled);
    s.setValue(QStringLiteral("udpAggregate"), m_udpAggregate);
    s.setValue(QStringLiteral("smoothing"), m_smoothing);
    s.setValue(QStringLiteral("directionColors"), m_directionColors);
    s.setValue(QStringLiteral("sortWithinGroups"), m_sortWithinGroups);
    s.setValue(QStringLiteral("showProcessColumn"), m_showProcessColumn);
    s.setValue(QStringLiteral("showContainerColumn"), m_showContainerColumn);
    s.setValue(QStringLiteral("pollMs"), m_pollMs);
    s.setValue(QStringLiteral("groupBy"), int(m_groupBy));
    if (!m_themes.isEmpty())
        s.setValue(QStringLiteral("theme"), m_themes[m_themeIdx].name);
    s.endGroup();
}

void TuiApp::handleSettingsKey(int key)
{
    const int n = m_settings.size();
    if (n == 0) { m_overlay = Overlay::None; requestRedraw(); return; }
    const auto activate = [this](int dir) {
        if (m_settingsSel >= 0 && m_settingsSel < m_settings.size())
            m_settings[m_settingsSel].adjust(dir);
        applyAggregatorSettings();
    };

    switch (key) {
    case 'S':
    case 'q':
    case 'Q':
    case 27: // Esc
        m_overlay = Overlay::None;
        break;
    case KEY_UP:
    case 'k':
        m_settingsSel = (m_settingsSel - 1 + n) % n;
        break;
    case KEY_DOWN:
    case 'j':
        m_settingsSel = (m_settingsSel + 1) % n;
        break;
    case KEY_LEFT:
    case 'h':
        activate(-1);
        break;
    case KEY_RIGHT:
    case 'l':
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        activate(+1);
        break;
    case KEY_RESIZE:
        break;
    default:
        return; // ignore, no repaint
    }
    requestRedraw();
}

void TuiApp::buildSettingItems()
{
    m_settings.clear();

    // Theme — cycle through the built-in palettes (wraps both directions).
    m_settings.append({
        QStringLiteral("Theme"),
        QStringLiteral("Colour palette. ←/→ cycles dark / light / colourblind / mono."),
        [this] { return m_themes.isEmpty() ? QString() : m_themes[m_themeIdx].name; },
        [this](int dir) {
            if (m_themes.isEmpty()) return;
            const int n = m_themes.size();
            m_themeIdx = (m_themeIdx + (dir >= 0 ? 1 : n - 1)) % n;
            if (m_screen) m_screen->setTheme(m_themes[m_themeIdx]);
        }});

    // Group connections — off / interface / process / container.
    m_settings.append({
        QStringLiteral("Group connections"),
        QStringLiteral("Group flows by interface / process / container (or off). Also: g."),
        [this] { return groupByName(m_groupBy); },
        [this](int dir) {
            const int n = static_cast<int>(GroupBy::Count);
            m_groupBy = static_cast<GroupBy>(
                (static_cast<int>(m_groupBy) + (dir >= 0 ? 1 : n - 1)) % n);
            m_connScroll = 0;
        }});

    // Poll interval — refresh cadence in ms (250-step, clamped 100..10000).
    m_settings.append({
        QStringLiteral("Poll interval"),
        QStringLiteral("Refresh cadence. ←/→ adjusts by 250 ms (100 ms – 10 s)."),
        [this] { return QStringLiteral("%1 ms").arg(m_pollMs); },
        [this](int dir) {
            m_pollMs = std::clamp(m_pollMs + dir * 250, 100, 10000);
            applyPollInterval();
        }});

    // Bandwidth gauge on/off.
    m_settings.append({
        QStringLiteral("Bandwidth gauge"),
        QStringLiteral("Row-spanning background bar scaled to the loudest flow (iftop-style)."),
        [this] { return onOff(m_gaugeEnabled); },
        [this](int) { m_gaugeEnabled = !m_gaugeEnabled; }});

    // Direction colours on/off.
    m_settings.append({
        QStringLiteral("Direction colours"),
        QStringLiteral("Tint rows by flow direction (outbound / inbound / forwarded)."),
        [this] { return onOff(m_directionColors); },
        [this](int) { m_directionColors = !m_directionColors; }});

    // Resolve hostnames on/off.
    m_settings.append({
        QStringLiteral("Resolve hostnames"),
        QStringLiteral("Reverse-DNS lookups for peer addresses (async, cached)."),
        [this] { return onOff(m_dnsEnabled); },
        [this](int) { m_dnsEnabled = !m_dnsEnabled; }});

    // Aggregate UDP by peer on/off.
    m_settings.append({
        QStringLiteral("Aggregate UDP by peer"),
        QStringLiteral("Collapse a peer's UDP flows into one row instead of per-port."),
        [this] { return onOff(m_udpAggregate); },
        [this](int) { m_udpAggregate = !m_udpAggregate; }});

    // Rate smoothing on/off.
    m_settings.append({
        QStringLiteral("Rate smoothing"),
        QStringLiteral("EMA-smooth the displayed rates so they ease between polls."),
        [this] { return onOff(m_smoothing); },
        [this](int) { m_smoothing = !m_smoothing; }});

    // Sort within groups on/off.
    m_settings.append({
        QStringLiteral("Sort within groups"),
        QStringLiteral("When grouped, sort rows inside each group and keep the group "
                       "order fixed; off = order groups by their total."),
        [this] { return onOff(m_sortWithinGroups); },
        [this](int) { m_sortWithinGroups = !m_sortWithinGroups; requestRedraw(); }});

    // Process column — show comm[pid] / attribution reason. Effective only when
    // the agent advertises process-attribution-wire (else "unavailable"). Also
    // toggleable from the Fields overlay (f); both route through the same flag.
    m_settings.append({
        QStringLiteral("Process column"),
        QStringLiteral("Show the Process column (comm[pid], or the reason a flow has "
                       "no local process). Needs an attribution-capable agent. Also: f."),
        [this] { return m_procWire ? onOff(m_showProcessColumn)
                                   : QStringLiteral("unavailable"); },
        [this](int) { toggleOptionalColumn(ColumnId::Process); requestRedraw(); }});

    // Container column — runtime:name. Effective only with container-attribution-wire.
    m_settings.append({
        QStringLiteral("Container column"),
        QStringLiteral("Show the Container column (runtime:name). Needs a "
                       "container-attribution-capable agent. Also: f."),
        [this] { return m_contWire ? onOff(m_showContainerColumn)
                                   : QStringLiteral("unavailable"); },
        [this](int) { toggleOptionalColumn(ColumnId::Container); requestRedraw(); }});
}

void TuiApp::applyPollInterval()
{
    if (m_connAgg)
        m_connAgg->setPollIntervalMs(m_pollMs);
    if (m_smoothTimer)
        m_smoothTimer->setInterval(std::max(100, m_pollMs / 4));
    if (m_applyPollMs)
        m_applyPollMs(m_pollMs);
}

void TuiApp::setPollApplier(std::function<void(int)> fn)
{
    m_applyPollMs = std::move(fn);
    applyPollInterval();   // sync the source to the (possibly persisted) interval
}

void TuiApp::setBackendInfo(bool usingAgent, const QString &version,
                            const QStringList &caps)
{
    m_usingAgent = usingAgent;
    m_backendCaps = caps;
    // Optional columns are gated on the ACTIVE backend's wire tokens —
    // transport-neutral, NOT agent-only. The in-process Linux conntrack path
    // advertises none (no resolver) and they stay hidden; the in-process BSD
    // path DOES attribute, so they light up just like the agent. No
    // `usingAgent` precondition. Without the token the columns would render
    // "—"/"(host)" everywhere, which is misleading rather than helpful.
    m_procWire = caps.contains(QStringLiteral("process-attribution-wire"));
    m_contWire = caps.contains(QStringLiteral("container-attribution-wire"));
    Q_UNUSED(version);
    requestRedraw();
}

OptionalColumns TuiApp::effectiveOptionalColumns() const
{
    if (m_view != View::Connections)
        return {};
    return {
        m_procWire && m_showProcessColumn   && m_groupBy != GroupBy::Process,
        m_contWire && m_showContainerColumn && m_groupBy != GroupBy::Container,
    };
}

QList<Column> TuiApp::activeColumns() const
{
    return columnsFor(m_view, effectiveOptionalColumns(), m_groupBy);
}

QList<Column> TuiApp::overlayColumns() const
{
    QList<Column> cols = overlayColumnsFor(m_view);
    for (Column &c : cols) {
        if (c.id == ColumnId::Process) {
            c.available = m_procWire;
            c.visible   = m_showProcessColumn;   // user pref; group-redundancy is separate
        } else if (c.id == ColumnId::Container) {
            c.available = m_contWire;
            c.visible   = m_showContainerColumn;
        }
    }
    return cols;
}

void TuiApp::cycleSortField()
{
    const QList<Column> cols = activeColumns();
    if (cols.isEmpty())
        return;
    int idx = visualIndexForColumn(cols, currentSortField());
    idx = (idx + 1) % static_cast<int>(cols.size());   // -1 (hidden) wraps to 0
    if (idx < 0)
        idx = 0;
    setCurrentSortField(cols[idx].id);
}

void TuiApp::toggleOptionalColumn(ColumnId id)
{
    if (id == ColumnId::Process)
        m_showProcessColumn = !m_showProcessColumn;
    else if (id == ColumnId::Container)
        m_showContainerColumn = !m_showContainerColumn;
}

void TuiApp::handleFieldsKey(int key)
{
    const QList<Column> cols = overlayColumns();
    const int n = static_cast<int>(cols.size());
    if (n == 0) { m_overlay = Overlay::None; requestRedraw(); return; }
    m_fieldsSel = std::clamp(m_fieldsSel, 0, n - 1);
    bool &sortDesc = (m_view == View::Interfaces) ? m_ifaceSortDesc : m_connSortDesc;
    const Column &sel = cols[m_fieldsSel];

    switch (key) {
    case 'f':
    case 'F':
    case 'q':
    case 'Q':
    case 27: // Esc
        m_overlay = Overlay::None;
        break;
    case KEY_UP:
    case 'k':
        m_fieldsSel = (m_fieldsSel - 1 + n) % n;
        break;
    case KEY_DOWN:
    case 'j':
        m_fieldsSel = (m_fieldsSel + 1) % n;
        break;
    case ' ':
        // Toggle visibility for an available optional field. Mandatory fields
        // are a no-op; an unavailable one flashes why it can't be shown.
        if (sel.hideable && sel.available)
            toggleOptionalColumn(sel.id);
        else if (sel.hideable && !sel.available)
            flashMessage(QStringLiteral("%1 needs the backend's %2 capability")
                             .arg(sel.title,
                                  sel.id == ColumnId::Container
                                      ? QStringLiteral("container-attribution-wire")
                                      : QStringLiteral("process-attribution-wire")));
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        // Make the selected field the sort field; if it already is, flip the
        // direction (preserves the old "activate current sort flips it" feel).
        if (currentSortField() == sel.id)
            sortDesc = !sortDesc;
        else
            setCurrentSortField(sel.id);
        break;
    case 'r':
    case 'R':
    case KEY_LEFT:
    case KEY_RIGHT:
        sortDesc = !sortDesc;
        break;
    case KEY_RESIZE:
        break;
    default:
        return; // ignore, no repaint
    }
    requestRedraw();
}

void TuiApp::handleInfoKey(int key)
{
    // Read-only Help/About panels: any key (except a terminal resize) closes.
    if (key == KEY_RESIZE) {
        requestRedraw();
        return;
    }
    m_overlay = Overlay::None;
    requestRedraw();
}

} // namespace qiftop::tui
