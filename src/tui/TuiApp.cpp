#include "tui/TuiApp.h"

#include "aggregate/ConnectionAggregator.h"
#include "aggregate/InterfaceAggregator.h"

#include <QCoreApplication>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>

// ncurses last (KEY_* constants); after Qt to avoid macro clashes.
#include <ncurses.h>

namespace qiftop::tui {

namespace {
constexpr int kRedrawThrottleMs = 33; // ~30 fps cap
constexpr auto kRepoUrl = "https://github.com/TheCleaners/qiftop";
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
    m_pollMs = std::max(1, pollMs);
    m_themes = builtinThemes();

    // Restore persisted view/sort/toggles/theme first, then let an explicit
    // --theme on the command line override the saved theme.
    loadSettings();
    if (!themeName.isEmpty()) {
        for (int i = 0; i < m_themes.size(); ++i) {
            if (m_themes[i].name.compare(themeName, Qt::CaseInsensitive) == 0) {
                m_themeIdx = i;
                break;
            }
        }
    }
    if (m_screen)
        m_screen->setTheme(m_themes[m_themeIdx]);
    applyAggregatorSettings();   // push restored DNS/UDP/smoothing toggles

    // Persist on any exit path (q, SIGINT, SIGTERM all route through quit()).
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { saveSettings(); });

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
    if (m_filterEditing) {
        handleFilterKey(key);
        return;
    }

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
    case 'S':
    case KEY_F(2):
        m_overlay = Overlay::Settings;
        break;
    case 'f':
    case 'F':
        m_fieldsSel = sortCol;          // start on the current sort column
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
        if (m_paused)
            m_smoothTimer->stop();
        else
            m_smoothTimer->start();
        break;
    case '/':
        m_filterEditing = true;
        m_filterDraft = m_filterText;   // edit the current filter in place
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
        sortCol = (sortCol + 1) % nCols;
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
    case 'G':
        m_groupBy = static_cast<GroupBy>((static_cast<int>(m_groupBy) + 1)
                                         % static_cast<int>(GroupBy::Count));
        m_connScroll = 0;
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

    QList<double> rates;       // combined rate per displayed row (for the gauge)
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

        const auto passesFilter = [&](const aggregate::ConnectionAggregator::Row &row) {
            if (!m_filterExpr)
                return true;
            const Connection &c = row.current;
            const qiftop::filter::Context ctx{
                c, row.rxRate, row.txRate,
                m_connAgg->cachedHostname(c.local.address),
                m_connAgg->cachedHostname(c.remote.address),
            };
            return qiftop::filter::matches(m_filterExpr, ctx);
        };

        // Filtered indices in the current sort order.
        QList<int> matched;
        for (int i : order)
            if (passesFilter(rows[i]))
                matched << i;

        if (m_groupBy == GroupBy::None) {
            for (int i : matched) {
                const auto &row = rows[i];
                f.rows     << cellsForConnection(*m_connAgg, row);
                f.rowRoles << rowRoleForConnection(*m_connAgg, row);
                const double cr = combinedRate(row);
                rates << cr;
                maxRate = std::max(maxRate, cr);
                aggRx += row.rxRate;
                aggTx += row.txRate;
            }
        } else {
            // Bucket by group key, preserving first-appearance order (which is
            // the sort order — so loudest-first when sorting by rate desc).
            QList<QString> groupOrder;
            QHash<QString, QList<int>> buckets;
            for (int i : matched) {
                const QString k = groupKeyFor(m_groupBy, rows[i].current);
                if (!buckets.contains(k))
                    groupOrder << k;
                buckets[k] << i;
            }
            for (const QString &k : std::as_const(groupOrder)) {
                const QList<int> &members = buckets[k];
                double grx = 0, gtx = 0;
                quint64 grb = 0, gtb = 0;
                for (int i : members) {
                    grx += rows[i].rxRate;
                    gtx += rows[i].txRate;
                    grb += rows[i].current.rxBytes;
                    gtb += rows[i].current.txBytes;
                }
                // Group header row (aggregated).
                const QString label = groupLabelFor(m_groupBy, rows[members.first()].current);
                f.rows << QStringList{
                    QStringLiteral("%1  (%2)").arg(label).arg(members.size()),
                    util::formatByteRate(grx), util::formatByteRate(gtx),
                    util::formatBytes(grb), util::formatBytes(gtb)};
                f.rowRoles << Role::GroupHeader;
                const double gcr = grx + gtx;
                rates << gcr;
                maxRate = std::max(maxRate, gcr);
                aggRx += grx;
                aggTx += gtx;
                // Member rows (indented in the flow column).
                for (int i : members) {
                    const auto &row = rows[i];
                    QStringList mc = cellsForConnection(*m_connAgg, row);
                    mc[0] = QStringLiteral("  ") + mc[0];
                    f.rows << mc;
                    f.rowRoles << rowRoleForConnection(*m_connAgg, row);
                    const double cr = combinedRate(row);
                    rates << cr;
                    maxRate = std::max(maxRate, cr);
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

    // Clamp scroll to the valid range for the current body height.
    const int body  = m_screen ? m_screen->bodyHeight() : 0;
    const int total = static_cast<int>(f.rows.size());
    int &scroll = (m_view == View::Interfaces) ? m_ifaceScroll : m_connScroll;
    const int maxScroll = std::max(0, total - body);
    scroll = std::clamp(scroll, 0, maxScroll);
    f.scrollOffset = scroll;

    if (m_filterEditing) {
        // The menu bar becomes the filter input line while editing.
        f.footer = m_filterError.isEmpty()
            ? QStringLiteral(" /%1\u2588   (Enter apply · Esc cancel)").arg(m_filterDraft)
            : QStringLiteral(" /%1\u2588   ! %2").arg(m_filterDraft, m_filterError);
    } else if (!m_filterText.isEmpty()) {
        f.footer = QStringLiteral(" filter: %1 · / edit · q quit · Tab view · s/f sort · ? help ")
                       .arg(m_filterText);
    } else {
        f.footer = QStringLiteral(
            " q quit · Tab view · s/f sort · g group · / filter · p pause · z theme · S settings · ? help ");
    }

    buildModal(f);
    return f;
}

void TuiApp::buildModal(Frame &f) const
{
    if (m_overlay == Overlay::Settings) {
        f.modal.visible  = true;
        f.modal.title    = QStringLiteral("Settings");
        f.modal.items    = settingsRows(m_themes.isEmpty() ? QString()
                                                           : m_themes[m_themeIdx].name,
                                         groupByName(m_groupBy),
                                         m_gaugeEnabled, m_dnsEnabled,
                                         m_udpAggregate, m_smoothing);
        f.modal.selected = m_settingsSel;
        f.modal.footer   = QStringLiteral("↑↓ move · ←/→/Space change · S/Esc close");
    } else if (m_overlay == Overlay::Fields) {
        const int sortCol  = (m_view == View::Interfaces) ? m_ifaceSortCol : m_connSortCol;
        const bool sortDesc = (m_view == View::Interfaces) ? m_ifaceSortDesc : m_connSortDesc;
        f.modal.visible  = true;
        f.modal.title    = QStringLiteral("Sort field");
        f.modal.items    = sortFieldRows(columnsFor(m_view), sortCol, sortDesc);
        f.modal.selected = m_fieldsSel;
        f.modal.footer   = QStringLiteral("↑↓ move · Enter/Space sort · r reverse · f/Esc close");
    } else if (m_overlay == Overlay::Help) {
        // Pre-format "key   description" into the label (read-only panel).
        const auto row = [](const QString &key, const QString &desc) {
            return SettingRow{key.leftJustified(14) + desc, QString(), QString()};
        };
        f.modal.visible    = true;
        f.modal.selectable = false;
        f.modal.title      = QStringLiteral("Help — key bindings");
        f.modal.items = {
            row(QStringLiteral("Tab / 1 / 2"), QStringLiteral("Switch view (Interfaces / Connections)")),
            row(QStringLiteral("↑ ↓"),         QStringLiteral("Scroll one row")),
            row(QStringLiteral("PgUp / PgDn"),  QStringLiteral("Scroll one page")),
            row(QStringLiteral("Home / End"),   QStringLiteral("Jump to top / bottom")),
            row(QStringLiteral("s"),            QStringLiteral("Cycle the sort column")),
            row(QStringLiteral("f"),            QStringLiteral("Fields: pick sort column & direction")),
            row(QStringLiteral("r"),            QStringLiteral("Reverse the sort order")),
            row(QStringLiteral("/"),            QStringLiteral("Filter connections (mini-language; Esc clears)")),
            row(QStringLiteral("g"),            QStringLiteral("Group connections by interface / process / container")),
            row(QStringLiteral("p"),            QStringLiteral("Pause / resume live updates")),
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
    m_ifaceSortCol  = s.value(QStringLiteral("ifaceSortCol"), m_ifaceSortCol).toInt();
    m_ifaceSortDesc = s.value(QStringLiteral("ifaceSortDesc"), m_ifaceSortDesc).toBool();
    m_connSortCol   = s.value(QStringLiteral("connSortCol"), m_connSortCol).toInt();
    m_connSortDesc  = s.value(QStringLiteral("connSortDesc"), m_connSortDesc).toBool();
    m_gaugeEnabled  = s.value(QStringLiteral("gauge"), m_gaugeEnabled).toBool();
    m_dnsEnabled    = s.value(QStringLiteral("dns"), m_dnsEnabled).toBool();
    m_udpAggregate  = s.value(QStringLiteral("udpAggregate"), m_udpAggregate).toBool();
    m_smoothing     = s.value(QStringLiteral("smoothing"), m_smoothing).toBool();
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
    // Clamp persisted sort columns in case the column set changed across versions.
    const int ifaceCols = static_cast<int>(columnsFor(View::Interfaces).size());
    const int connCols  = static_cast<int>(columnsFor(View::Connections).size());
    m_ifaceSortCol = std::clamp(m_ifaceSortCol, 0, ifaceCols - 1);
    m_connSortCol  = std::clamp(m_connSortCol, 0, connCols - 1);
}

void TuiApp::saveSettings() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("nqiftop"));
    s.setValue(QStringLiteral("view"), int(m_view));
    s.setValue(QStringLiteral("ifaceSortCol"), m_ifaceSortCol);
    s.setValue(QStringLiteral("ifaceSortDesc"), m_ifaceSortDesc);
    s.setValue(QStringLiteral("connSortCol"), m_connSortCol);
    s.setValue(QStringLiteral("connSortDesc"), m_connSortDesc);
    s.setValue(QStringLiteral("gauge"), m_gaugeEnabled);
    s.setValue(QStringLiteral("dns"), m_dnsEnabled);
    s.setValue(QStringLiteral("udpAggregate"), m_udpAggregate);
    s.setValue(QStringLiteral("smoothing"), m_smoothing);
    s.setValue(QStringLiteral("groupBy"), int(m_groupBy));
    if (!m_themes.isEmpty())
        s.setValue(QStringLiteral("theme"), m_themes[m_themeIdx].name);
    s.endGroup();
}

void TuiApp::handleSettingsKey(int key)
{
    const int n = static_cast<int>(Setting::Count);
    const auto change = [this] {
        switch (static_cast<Setting>(m_settingsSel)) {
        case Setting::Theme:
            if (!m_themes.isEmpty()) {
                m_themeIdx = (m_themeIdx + 1) % m_themes.size();
                if (m_screen) m_screen->setTheme(m_themes[m_themeIdx]);
            }
            break;
        case Setting::GroupBy:
            m_groupBy = static_cast<GroupBy>((static_cast<int>(m_groupBy) + 1)
                                             % static_cast<int>(GroupBy::Count));
            m_connScroll = 0;
            break;
        case Setting::Gauge:        m_gaugeEnabled = !m_gaugeEnabled; break;
        case Setting::Dns:          m_dnsEnabled   = !m_dnsEnabled;   break;
        case Setting::UdpAggregate: m_udpAggregate = !m_udpAggregate; break;
        case Setting::Smoothing:    m_smoothing    = !m_smoothing;    break;
        case Setting::Count:        break;
        }
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
        m_settingsSel = (m_settingsSel - 1 + n) % n;
        break;
    case KEY_DOWN:
        m_settingsSel = (m_settingsSel + 1) % n;
        break;
    case KEY_LEFT:
    case KEY_RIGHT:
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        change();
        break;
    case KEY_RESIZE:
        break;
    default:
        return; // ignore, no repaint
    }
    requestRedraw();
}

void TuiApp::handleFieldsKey(int key)
{
    const int nCols = static_cast<int>(columnsFor(m_view).size());
    int &sortCol = (m_view == View::Interfaces) ? m_ifaceSortCol : m_connSortCol;
    bool &sortDesc = (m_view == View::Interfaces) ? m_ifaceSortDesc : m_connSortDesc;

    switch (key) {
    case 'f':
    case 'F':
    case 'q':
    case 'Q':
    case 27: // Esc
        m_overlay = Overlay::None;
        break;
    case KEY_UP:
        m_fieldsSel = (m_fieldsSel - 1 + nCols) % nCols;
        break;
    case KEY_DOWN:
        m_fieldsSel = (m_fieldsSel + 1) % nCols;
        break;
    case ' ':
    case '\n':
    case '\r':
    case KEY_ENTER:
        // Selecting the current sort column toggles direction; otherwise make
        // the highlighted column the sort column (keeping the direction).
        if (sortCol == m_fieldsSel)
            sortDesc = !sortDesc;
        else
            sortCol = m_fieldsSel;
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
