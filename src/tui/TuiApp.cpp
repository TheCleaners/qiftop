#include "tui/TuiApp.h"

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"

#include <QCoreApplication>
#include <QTimer>

#include <algorithm>

// ncurses last (KEY_* constants); after Qt to avoid macro clashes.
#include <ncurses.h>

namespace qiftop::tui {

namespace {
constexpr int kRedrawThrottleMs = 33; // ~30 fps cap
}

TuiApp::TuiApp(Screen *screen,
               aggregate::InterfaceAggregator  *ifaceAgg,
               aggregate::ConnectionAggregator *connAgg,
               QString sourceLabel,
               QString themeName,
               int pollMs,
               QObject *parent)
    : QObject(parent)
    , m_screen(screen)
    , m_ifaceAgg(ifaceAgg)
    , m_connAgg(connAgg)
    , m_sourceLabel(std::move(sourceLabel))
{
    m_themes = builtinThemes();
    for (int i = 0; i < m_themes.size(); ++i) {
        if (m_themes[i].name.compare(themeName, Qt::CaseInsensitive) == 0) {
            m_themeIdx = i;
            break;
        }
    }
    if (m_screen)
        m_screen->setTheme(m_themes[m_themeIdx]);

    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setSingleShot(true);
    connect(m_redrawTimer, &QTimer::timeout, this, &TuiApp::doRedraw);

    // Smoothing tick: advance the display tween toward the EMA target between
    // polls (mirrors the GUI). The aggregator emits rowsUpdated -> redraw.
    m_smoothTimer = new QTimer(this);
    m_smoothTimer->setInterval(std::max(100, pollMs / 4));
    connect(m_smoothTimer, &QTimer::timeout, this,
            [this] { if (m_connAgg) m_connAgg->advanceSmoothing(); });
    m_smoothTimer->start();

    // Any data change -> coalesced repaint.
    connect(m_ifaceAgg, &aggregate::InterfaceAggregator::didReset,
            this, &TuiApp::requestRedraw);
    connect(m_ifaceAgg, &aggregate::InterfaceAggregator::rowsChanged,
            this, [this](int, int) { requestRedraw(); });
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsInserted,
            this, &TuiApp::requestRedraw);
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsRemoved,
            this, &TuiApp::requestRedraw);
    connect(m_connAgg, &aggregate::ConnectionAggregator::rowsUpdated,
            this, [this](int, int) { requestRedraw(); });
    connect(m_connAgg, &aggregate::ConnectionAggregator::viewDataChanged,
            this, &TuiApp::requestRedraw);

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
    const int nCols = static_cast<int>(columnsFor(m_view).size());
    int &scroll = (m_view == View::Interfaces) ? m_ifaceScroll : m_connScroll;
    int &sortCol = (m_view == View::Interfaces) ? m_ifaceSortCol : m_connSortCol;
    bool &sortDesc = (m_view == View::Interfaces) ? m_ifaceSortDesc : m_connSortDesc;

    switch (key) {
    case 'q':
    case 'Q':
        if (m_screen) m_screen->shutdown();
        QCoreApplication::quit();
        return;
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
    case 'S':
        sortCol = (sortCol + 1) % nCols;
        break;
    case 'r':
    case 'R':
        sortDesc = !sortDesc;
        break;
    case 't':
    case 'T':
        if (!m_themes.isEmpty()) {
            m_themeIdx = (m_themeIdx + 1) % m_themes.size();
            if (m_screen)
                m_screen->setTheme(m_themes[m_themeIdx]);
        }
        break;
    case KEY_UP:
        if (scroll > 0) --scroll;
        break;
    case KEY_DOWN:
        ++scroll; // clamped in buildFrame
        break;
    case KEY_PPAGE:
        scroll = std::max(0, scroll - std::max(1, m_screen ? m_screen->bodyHeight() : 1));
        break;
    case KEY_NPAGE:
        scroll += std::max(1, m_screen ? m_screen->bodyHeight() : 1);
        break;
    case KEY_HOME:
        scroll = 0;
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
    f.columns     = columnsFor(m_view);

    QList<double> rates;       // combined rate per displayed row (for the bar)
    double maxRate = 0.0;
    double aggRx = 0.0, aggTx = 0.0;

    if (m_view == View::Interfaces) {
        f.sortCol  = m_ifaceSortCol;
        f.sortDesc = m_ifaceSortDesc;
        const auto &rows = m_ifaceAgg->rows();
        const QList<int> order =
            sortedInterfaceIndices(rows, m_ifaceSortCol, m_ifaceSortDesc);
        for (int i : order) {
            f.rows     << cellsForInterface(rows[i]);
            f.rowRoles << rowRoleForInterface(rows[i]);
            const double cr = combinedRate(rows[i]);
            rates << cr;
            maxRate = std::max(maxRate, cr);
            aggRx += rows[i].rxRate;
            aggTx += rows[i].txRate;
        }
    } else {
        f.sortCol  = m_connSortCol;
        f.sortDesc = m_connSortDesc;
        const auto &rows = m_connAgg->rows();
        const QList<int> order =
            sortedConnectionIndices(rows, m_connSortCol, m_connSortDesc);
        for (int i : order) {
            f.rows     << cellsForConnection(*m_connAgg, rows[i]);
            f.rowRoles << rowRoleForConnection(*m_connAgg, rows[i]);
            const double cr = combinedRate(rows[i]);
            rates << cr;
            maxRate = std::max(maxRate, cr);
            aggRx += rows[i].rxRate;
            aggTx += rows[i].txRate;
        }
    }

    // Bandwidth gauge: scale a full bar to a "nice" round value >= the loudest
    // row (iftop's scale), then fill each row's bar cell.
    const double scale = niceScale(maxRate);
    f.columns[kBarColumn].title = QStringLiteral("\u2264%1").arg(util::formatByteRate(scale));
    for (int k = 0; k < f.rows.size() && k < rates.size(); ++k)
        f.rows[k][kBarColumn] = barString(rates[k], scale, kBarWidth);

    // Aggregate throughput in the tab-line right gutter, next to the source.
    f.sourceLabel = QStringLiteral("\u03a3 %1\u2193 %2\u2191 \u00b7 %3")
                        .arg(util::formatByteRate(aggRx),
                             util::formatByteRate(aggTx),
                             m_sourceLabel);

    // Clamp scroll to the valid range for the current body height.
    const int body  = m_screen ? m_screen->bodyHeight() : 0;
    const int total = static_cast<int>(f.rows.size());
    int &scroll = (m_view == View::Interfaces) ? m_ifaceScroll : m_connScroll;
    const int maxScroll = std::max(0, total - body);
    scroll = std::clamp(scroll, 0, maxScroll);
    f.scrollOffset = scroll;

    f.footer = QStringLiteral(
        " q quit · Tab/1/2 view · s sort · r reverse · t theme · ↑↓/PgUp/PgDn scroll ");
    return f;
}

} // namespace qiftop::tui
