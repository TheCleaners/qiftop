#include "MainWindow.h"

#include <algorithm>

#include "ConnectionFilterProxy.h"
#include "ConnectionFlowDelegate.h"
#include "ConnectionModel.h"
#include "InterfaceFilterProxy.h"
#include "InterfaceNameDelegate.h"
#include "NetworkModel.h"
#include "RowGaugeDelegate.h"
#include "SettingsDialog.h"
#include "TrayManager.h"
#include "config/Settings.h"
#include "dns/DnsResolver.h"
#include "util/Exportable.h"
#include "util/Exporter.h"
#include "util/ConnectionFilter.h"
#include "util/HandoffClient.h"
#include "util/HandoffServer.h"
#include "util/Logging.h"
#include "util/PrivilegeEscalator.h"
#include "util/Units.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QSizePolicy>
#include <QLineEdit>
#include <QToolTip>
#include <QVBoxLayout>

#include <memory>

MainWindow::MainWindow(Settings          *settings,
                       NetworkMonitor    *netMonitor,
                       ConnectionMonitor *connMonitor,
                       DnsResolver       *dnsResolver,
                       QWidget           *parent)
    : QMainWindow(parent)
    , m_settings(settings)
    , m_netMonitor(netMonitor)
    , m_connMonitor(connMonitor)
    , m_dnsResolver(dnsResolver)
{
    setupUi();
    setupMenuAndToolbar();
    applySettingsToUi();
    readUiState();

    // Toolbar button + View submenu for the iface filter only apply on
    // the Connections tab; toggle their state whenever the tab changes,
    // and once at startup.
    connect(m_tabs, &QTabWidget::currentChanged,
            this,   [this](int) { updateConnIfaceFilterVisibility(); });
    updateConnIfaceFilterVisibility();

    // Right-click on interface rows -> "Add to/Remove from tray summary".
    m_netView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_netView, &QWidget::customContextMenuRequested,
            this,      &MainWindow::showInterfaceContextMenu);

    // Tray. On freshly-spawned privileged instances the StatusNotifier host
    // (panel / SNI watcher on the user's session bus) often isn't reachable
    // yet — root may need a moment to authenticate against the user's DBus,
    // or the panel hasn't noticed us yet. Retry availability for up to ~30s.
    m_tray = new TrayManager(m_settings, this);
    connect(m_tray, &TrayManager::showWindowRequested, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    connect(m_tray, &TrayManager::pauseToggled, m_pauseAction, &QAction::setChecked);
    connect(m_tray, &TrayManager::quitRequested, this, &MainWindow::quitFromTray);

    auto tryShowTray = [this]() -> bool {
        if (!m_tray->isAvailable()) return false;
        m_tray->setVisible(true);
        qCInfo(lcVerbose) << "tray: visible";
        // Tray just became available: refresh anything keyed off it
        // (e.g. File → "Close to tray" visibility).
        applySettingsToUi();
        return true;
    };
    if (!tryShowTray()) {
        qCInfo(lcVerbose) << "tray: not available at startup; will retry";
        auto *retry = new QTimer(this);
        retry->setInterval(1000);
        auto attempt = std::make_shared<int>(0);
        connect(retry, &QTimer::timeout, this, [this, retry, tryShowTray, attempt] {
            if (tryShowTray() || ++(*attempt) >= 30) {
                if (*attempt >= 30)
                    qCWarning(lcVerbose) << "tray: gave up after 30 retries "
                                            "(no StatusNotifier host on session bus?)";
                retry->stop();
                retry->deleteLater();
            }
        });
        retry->start();
    }

    // Persist UI state on real application quit (covers hide-to-tray scenarios).
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::writeUiState);

    connect(m_settings,    &Settings::changed,
            this,          &MainWindow::onSettingsChanged);
    connect(m_netMonitor,  &NetworkMonitor::statsUpdated,
            this,          &MainWindow::onStatsUpdated);
    connect(m_connMonitor, &ConnectionMonitor::connectionsUpdated,
            this,          &MainWindow::onConnectionsUpdated);
    connect(m_connMonitor, &ConnectionMonitor::permissionDenied,
            this,          &MainWindow::onConnectionsPermissionDenied);
    connect(m_connMonitor, &ConnectionMonitor::accountingUnavailable,
            this,          &MainWindow::onConnectionsAccountingUnavailable);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    setWindowTitle(tr("qiftop — Network Monitor"));
    resize(900, 560);

    // --- Interfaces tab ---
    m_netModel = new NetworkModel(this);
    m_netProxy = new InterfaceFilterProxy(this);
    m_netProxy->setSourceModel(m_netModel);
    m_netProxy->setSortRole(NetworkModel::SortRole);

    m_netView = new QTableView;
    m_netView->setModel(m_netProxy);
    m_netView->setSortingEnabled(true);
    m_netView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_netView->setAlternatingRowColors(true);
    m_netView->verticalHeader()->setVisible(false);
    m_netView->setShowGrid(false);
    m_netView->horizontalHeader()->setStretchLastSection(false);
    m_netView->horizontalHeader()->setSectionResizeMode(
        static_cast<int>(NetworkModel::Column::Name), QHeaderView::Stretch);
    auto *ifaceDelegate = new InterfaceNameDelegate(m_netView);
    ifaceDelegate->setSettings(m_settings);
    m_netView->setItemDelegateForColumn(
        static_cast<int>(NetworkModel::Column::Name), ifaceDelegate);
    // Repaint when the tray-summary selection changes so the eye glyph updates.
    connect(m_settings, &Settings::changed, m_netView, [this] {
        m_netView->viewport()->update();
    });
    m_netView->sortByColumn(static_cast<int>(NetworkModel::Column::Name),
                            Qt::AscendingOrder);

    // --- Connections tab ---
    m_connModel = new ConnectionModel(this);
    m_connModel->setDnsResolver(m_dnsResolver);

    m_connProxy = new ConnectionFilterProxy(this);
    m_connProxy->setSourceModel(m_connModel);
    m_connProxy->setSortRole(ConnectionModel::SortRole);

    m_connView = new QTableView;
    m_connView->setModel(m_connProxy);
    m_connView->setSortingEnabled(true);
    m_connView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connView->setAlternatingRowColors(true);
    m_connView->verticalHeader()->setVisible(false);
    m_connView->setShowGrid(false);
    m_connView->horizontalHeader()->setStretchLastSection(false);
    m_connView->horizontalHeader()->setSectionResizeMode(
        static_cast<int>(ConnectionModel::Column::Flow), QHeaderView::Stretch);
    m_connFlowDelegate = new ConnectionFlowDelegate(m_connView);
    m_connView->setItemDelegateForColumn(
        static_cast<int>(ConnectionModel::Column::Flow),
        m_connFlowDelegate);
    // Default delegate for the other columns: paints the row-spanning
    // throughput gauge background before chaining to the standard styled
    // item rendering. Owned by the view (parent).
    m_connView->setItemDelegate(new RowGaugeDelegate(m_connView, m_connView));
    // Max columns are only meaningful with the gauge enabled; hide by
    // default and (un)hide in applySettingsToUi() based on the setting.
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::RxMax), true);
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::TxMax), true);
    m_connView->sortByColumn(static_cast<int>(ConnectionModel::Column::RxRate),
                             Qt::DescendingOrder);
    m_connView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connView, &QWidget::customContextMenuRequested,
            this,        &MainWindow::showConnectionContextMenu);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_netView,  tr("Interfaces"));

    // Wrap the connections view in a vbox so we can stack a privilege banner
    // above it without restructuring the tab widget later.
    auto *connTab    = new QWidget;
    m_connTab        = connTab;
    auto *connLayout = new QVBoxLayout(connTab);
    connLayout->setContentsMargins(0, 0, 0, 0);
    connLayout->setSpacing(0);

    m_connBanner = new QFrame;
    m_connBanner->setObjectName(QStringLiteral("connBanner"));
    m_connBanner->setFrameShape(QFrame::StyledPanel);
    m_connBanner->setStyleSheet(QStringLiteral(
        "QFrame#connBanner {"
        "  background: palette(highlight);"
        "  border: none;"
        "  padding: 6px;"
        "}"
        "QFrame#connBanner QLabel { color: palette(highlighted-text); }"));
    auto *bannerLayout = new QHBoxLayout(m_connBanner);
    bannerLayout->setContentsMargins(8, 4, 8, 4);
    m_connBannerLbl = new QLabel(m_connBanner);
    m_connBannerLbl->setWordWrap(true);
    auto *relaunchBtn = new QPushButton(
        QIcon::fromTheme(QStringLiteral("system-lock-screen")),
        tr("Relaunch as administrator"), m_connBanner);
    connect(relaunchBtn, &QPushButton::clicked, this, &MainWindow::relaunchAsAdmin);
    bannerLayout->addWidget(m_connBannerLbl, /*stretch*/ 1);
    bannerLayout->addWidget(relaunchBtn);
    m_connBanner->setVisible(false);

    connLayout->addWidget(m_connBanner);
    connLayout->addWidget(m_connView);

    m_tabs->addTab(connTab, tr("Connections"));
    setCentralWidget(m_tabs);

    // --- Status bar ---
    m_statusInterfaces  = new QLabel(tr("0 interfaces"));
    m_statusConnections = new QLabel(tr("0 connections"));
    m_statusThroughput  = new QLabel(QStringLiteral("↓ 0 B   ↑ 0 B"));
    m_statusBackend     = new QLabel(tr("backend: unknown"));
    m_statusBackend->setToolTip(tr("Active data source. Hover the value once "
                                   "set for the agent version and capabilities."));
    statusBar()->addPermanentWidget(m_statusBackend);
    statusBar()->addPermanentWidget(m_statusInterfaces);
    statusBar()->addPermanentWidget(m_statusConnections);
    statusBar()->addPermanentWidget(m_statusThroughput);
}

void MainWindow::setupMenuAndToolbar()
{
    // --- Build the shared export actions first; both menus reference them. ---
    m_exportIfacesJsonAct = new QAction(tr("Interfaces as &JSON…"), this);
    m_exportIfacesCsvAct  = new QAction(tr("Interfaces as &CSV…"),  this);
    m_exportConnsJsonAct  = new QAction(tr("Connections as J&SON…"), this);
    m_exportConnsCsvAct   = new QAction(tr("Connections as CS&V…"),  this);
    m_copyIfacesJsonAct   = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                        tr("&Interfaces as JSON"), this);
    m_copyIfacesCsvAct    = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                        tr("I&nterfaces as CSV"),  this);
    m_copyConnsJsonAct    = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                        tr("&Connections as JSON"), this);
    m_copyConnsCsvAct     = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                        tr("Co&nnections as CSV"),  this);
    // Wire up export / copy actions via lambdas — each call site differs
    // only by (model, format, sink, basename), so a single helper keeps
    // the slot machinery footprint down.
    using EF = ExportFormat;
    using ES = ExportSink;
    connect(m_exportIfacesJsonAct, &QAction::triggered, this, [this] {
        runExport(m_netModel,  EF::Json, ES::File,      QStringLiteral("interfaces"));  });
    connect(m_exportIfacesCsvAct,  &QAction::triggered, this, [this] {
        runExport(m_netModel,  EF::Csv,  ES::File,      QStringLiteral("interfaces"));  });
    connect(m_exportConnsJsonAct,  &QAction::triggered, this, [this] {
        runExport(m_connModel, EF::Json, ES::File,      QStringLiteral("connections")); });
    connect(m_exportConnsCsvAct,   &QAction::triggered, this, [this] {
        runExport(m_connModel, EF::Csv,  ES::File,      QStringLiteral("connections")); });
    connect(m_copyIfacesJsonAct,   &QAction::triggered, this, [this] {
        runExport(m_netModel,  EF::Json, ES::Clipboard, QStringLiteral("interfaces"));  });
    connect(m_copyIfacesCsvAct,    &QAction::triggered, this, [this] {
        runExport(m_netModel,  EF::Csv,  ES::Clipboard, QStringLiteral("interfaces"));  });
    connect(m_copyConnsJsonAct,    &QAction::triggered, this, [this] {
        runExport(m_connModel, EF::Json, ES::Clipboard, QStringLiteral("connections")); });
    connect(m_copyConnsCsvAct,     &QAction::triggered, this, [this] {
        runExport(m_connModel, EF::Csv,  ES::Clipboard, QStringLiteral("connections")); });

    // --- Menus ---
    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *exportMenu = fileMenu->addMenu(
        QIcon::fromTheme(QStringLiteral("document-save-as")), tr("&Export"));
    exportMenu->addAction(m_exportIfacesJsonAct);
    exportMenu->addAction(m_exportIfacesCsvAct);
    exportMenu->addSeparator();
    exportMenu->addAction(m_exportConnsJsonAct);
    exportMenu->addAction(m_exportConnsCsvAct);

    auto *copyMenu = fileMenu->addMenu(
        QIcon::fromTheme(QStringLiteral("edit-copy")), tr("&Copy to clipboard"));
    copyMenu->addAction(m_copyIfacesJsonAct);
    copyMenu->addAction(m_copyIfacesCsvAct);
    copyMenu->addSeparator();
    copyMenu->addAction(m_copyConnsJsonAct);
    copyMenu->addAction(m_copyConnsCsvAct);

    fileMenu->addSeparator();
    // "Close" — hides the main window to the tray. Only meaningful (and
    // only visible) when closeToTray is on AND the tray is actually
    // available. Visibility is refreshed in applySettingsToUi().
    m_closeAction = fileMenu->addAction(
        QIcon::fromTheme(QStringLiteral("window-close")),
        tr("&Close to tray"));
    m_closeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    m_closeAction->setShortcutContext(Qt::ApplicationShortcut);
    m_closeAction->setToolTip(tr("Hide the main window to the system tray"));
    connect(m_closeAction, &QAction::triggered, this, &QWidget::close);
    m_closeAction->setVisible(false);

    m_quitAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                       tr("&Quit"), this, &MainWindow::quitFromTray);
    // Use Ctrl+Q explicitly (which happens to match QKeySequence::Quit on
    // Linux/Win; macOS users still get Cmd+Q from system-wide menu key
    // equivalents). Application-scoped so the shortcut keeps firing
    // when the menu bar or toolbar is hidden — actions parented to a
    // hidden QMenuBar otherwise drop out of Qt's shortcut routing.
    m_quitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));
    m_quitAction->setShortcutContext(Qt::ApplicationShortcut);

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_settingsAction = editMenu->addAction(
        QIcon::fromTheme(QStringLiteral("preferences-system")),
        tr("&Preferences…"), this, &MainWindow::openSettingsDialog);
    m_settingsAction->setShortcut(QKeySequence::Preferences);
    m_settingsAction->setShortcutContext(Qt::ApplicationShortcut);
    m_settingsAction->setToolTip(tr("Preferences"));

    // --- Toolbar ---
    auto *toolbar = addToolBar(tr("Main"));
    m_toolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("MainToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_pauseAction = toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("media-playback-pause")), tr("Pause"));
    m_pauseAction->setCheckable(true);
    m_pauseAction->setToolTip(tr("Pause updates"));
    connect(m_pauseAction, &QAction::toggled, this, &MainWindow::togglePaused);

    toolbar->addSeparator();

    // Export tool button: icon with an instant-popup format menu, sharing the
    // same QActions as the File→Export submenu.
    auto *exportBtn = new QToolButton(toolbar);
    exportBtn->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as")));
    exportBtn->setText(tr("Export"));
    exportBtn->setToolTip(tr("Export…"));
    exportBtn->setPopupMode(QToolButton::InstantPopup);
    auto *exportBtnMenu = new QMenu(exportBtn);
    exportBtnMenu->addAction(m_exportIfacesJsonAct);
    exportBtnMenu->addAction(m_exportIfacesCsvAct);
    exportBtnMenu->addSeparator();
    exportBtnMenu->addAction(m_exportConnsJsonAct);
    exportBtnMenu->addAction(m_exportConnsCsvAct);
    exportBtnMenu->addSeparator();
    auto *copyBtnSubmenu = exportBtnMenu->addMenu(
        QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy to clipboard"));
    copyBtnSubmenu->addAction(m_copyIfacesJsonAct);
    copyBtnSubmenu->addAction(m_copyIfacesCsvAct);
    copyBtnSubmenu->addSeparator();
    copyBtnSubmenu->addAction(m_copyConnsJsonAct);
    copyBtnSubmenu->addAction(m_copyConnsCsvAct);
    exportBtn->setMenu(exportBtnMenu);
    toolbar->addWidget(exportBtn);

    toolbar->addSeparator();
    toolbar->addAction(m_settingsAction);

    // --- "Show connections on" filter -----------------------------------
    // Single QMenu, owned by the window, installed both as a popup on a
    // toolbar QToolButton (visible only when the Connections tab is
    // active) and as a submenu in the View menu (enabled only when the
    // Connections tab is active). Contents are rebuilt on every stats
    // tick by rebuildConnIfaceFilterMenu().
    m_connIfaceFilterMenu = new QMenu(tr("Show connections on"), this);

    toolbar->addSeparator();
    m_connIfaceFilterBtn = new QToolButton(toolbar);
    m_connIfaceFilterBtn->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    m_connIfaceFilterBtn->setText(tr("All interfaces"));
    m_connIfaceFilterBtn->setToolTip(tr("Show connections on…"));
    m_connIfaceFilterBtn->setPopupMode(QToolButton::MenuButtonPopup);
    m_connIfaceFilterBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_connIfaceFilterBtn->setAutoRaise(false); // render as a discrete button
    m_connIfaceFilterBtn->setMenu(m_connIfaceFilterMenu);
    // Clicking the main button face (not the arrow) just pops the menu too.
    connect(m_connIfaceFilterBtn, &QToolButton::clicked, m_connIfaceFilterBtn,
            &QToolButton::showMenu);
    m_connIfaceFilterToolbarAct = toolbar->addWidget(m_connIfaceFilterBtn);

    // Visual breathing room between the iface dropdown and the filter
    // line edit. QToolBar::addSeparator() draws a vertical divider which
    // also pads either side by the style's default spacing.
    m_connFilterSepAct = toolbar->addSeparator();

    // Expanding spacer: pushes the Filter group to the right edge of
    // the toolbar. When the window is too narrow to accommodate
    // everything, Qt collapses this spacer first (it has 0 minimum
    // width) so the filter falls back to sitting next to the iface
    // dropdown rather than being clipped.
    auto *spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_connFilterSpacerAct = toolbar->addWidget(spacer);

    // Free-form filter expression bar. Sits next to the iface filter and
    // is only meaningful on the Connections tab — visibility tracks the
    // current tab via updateConnIfaceFilterVisibility(). The trailing
    // "?" action pops a persistent syntax cheat-sheet popup.
    auto *filterBox       = new QWidget(toolbar);
    filterBox->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *filterLayout    = new QHBoxLayout(filterBox);
    filterLayout->setContentsMargins(4, 0, 0, 0);
    filterLayout->setSpacing(6);
    auto *filterLabel     = new QLabel(tr("Filter:"), filterBox);
    filterLabel->setForegroundRole(QPalette::WindowText);
    m_connFilterEdit      = new QLineEdit(filterBox);
    m_connFilterEdit->setClearButtonEnabled(true);
    m_connFilterEdit->setPlaceholderText(
        tr("e.g.  proto:tcp and dport=443"));
    m_connFilterEdit->setMinimumWidth(240);
    m_connFilterEdit->setMaximumWidth(440);
    m_connFilterEdit->setToolTip(tr("Filter expression — click ? for syntax"));
    QAction *helpAct = m_connFilterEdit->addAction(
        QIcon::fromTheme(QStringLiteral("help-contextual"),
                         QIcon::fromTheme(QStringLiteral("help-about"))),
        QLineEdit::TrailingPosition);
    helpAct->setToolTip(tr("Filter syntax help"));
    connect(helpAct, &QAction::triggered, this, &MainWindow::showFilterHelp);
    filterLayout->addWidget(filterLabel);
    filterLayout->addWidget(m_connFilterEdit);
    connect(m_connFilterEdit, &QLineEdit::textChanged, this,
            &MainWindow::onConnFilterTextChanged);
    m_connFilterToolbarAct = toolbar->addWidget(filterBox);

    // View menu: menu-bar + toolbar visibility toggles, pause mirror,
    // and the per-tab iface-filter submenu. The toolbar's built-in
    // toggleViewAction() defaults to the toolbar's title ("Main") which
    // is opaque to users — rename it to "Show Toolbar". Menu bar has no
    // built-in toggle action, so we wire one up by hand and resync its
    // checked state on aboutToShow (handles users hiding the bar via
    // any of the other paths — context menus, etc.).
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    m_menuBarToggleAction = viewMenu->addAction(tr("Show &Menu Bar"));
    m_menuBarToggleAction->setCheckable(true);
    m_menuBarToggleAction->setChecked(menuBar()->isVisible());
    connect(m_menuBarToggleAction, &QAction::toggled, this, [this](bool on) {
        if (QMenuBar *bar = menuBar())
            bar->setVisible(on);
    });
    QAction *tbToggle = toolbar->toggleViewAction();
    tbToggle->setText(tr("Show &Toolbar"));
    viewMenu->addAction(tbToggle);
    connect(viewMenu, &QMenu::aboutToShow, this, [this] {
        if (m_menuBarToggleAction)
            m_menuBarToggleAction->setChecked(menuBar()->isVisible());
    });
    viewMenu->addSeparator();
    viewMenu->addAction(m_pauseAction);
    viewMenu->addSeparator();
    m_connIfaceFilterMenuAct = viewMenu->addMenu(m_connIfaceFilterMenu);
}

void MainWindow::applySettingsToUi()
{
    m_netProxy->setShowLoopback(m_settings->showLoopback());
    m_netProxy->setShowDown(m_settings->showDown());
    m_connProxy->setShowIPv6(m_settings->ipv6Enabled());
    m_connProxy->setShowTcp(m_settings->showTcp());
    m_connProxy->setShowUdp(m_settings->showUdp());
    m_connModel->setHostnameResolutionEnabled(m_settings->resolveHostnames());
    m_connModel->setResolveIfaceAddrsAsLocalhost(m_settings->resolveIfaceAddrsAsLocalhost());
    m_connModel->setStaleRetentionMs(m_settings->connectionStaleRetentionSecs() * 1000);
    m_connModel->setStaleRetentionMsUdp(m_settings->connectionStaleRetentionSecsUdp() * 1000);
    m_connModel->setUdpAggregateByPeer(m_settings->udpAggregateByPeer());
    // Row-tint requires color-coding to be on; otherwise force off so the
    // two settings don't drift out of sync with the dialog's grayed-out
    // semantics.
    const bool tint = m_settings->colorCodeConnectionFlow()
                   && m_settings->tintRowByDirection();
    m_connModel->setTintRowByDirection(tint);
    // Throughput gauge: feed model state + show/hide the Max columns.
    m_connModel->setThroughputGaugeEnabled(m_settings->throughputGaugeEnabled());
    m_connModel->setThroughputMaxMode(
        static_cast<ConnectionModel::ThroughputMaxMode>(
            static_cast<int>(m_settings->throughputMaxMode())));
    m_connModel->setThroughputWindowMs(m_settings->throughputWindowSecs() * 1000);
    m_connModel->setRateSmoothingMs(m_settings->rateSmoothingMs());
    m_connModel->setPollIntervalMs(m_settings->pollIntervalMs());
    if (m_closeAction) {
        // "Close to tray" file-menu entry only makes sense when the
        // setting is on AND a tray host is actually available; otherwise
        // triggering it would just block forever waiting for the tray.
        const bool trayOk = m_tray && m_tray->isAvailable();
        m_closeAction->setVisible(m_settings->closeToTray() && trayOk);
    }
    if (m_connView) {
        const bool showMax = m_settings->throughputGaugeEnabled();
        m_connView->setColumnHidden(
            static_cast<int>(ConnectionModel::Column::RxMax), !showMax);
        m_connView->setColumnHidden(
            static_cast<int>(ConnectionModel::Column::TxMax), !showMax);
    }
    if (m_connFlowDelegate) {
        const bool wasOn = m_connFlowDelegate->colorCodeEnabled();
        const bool nowOn = m_settings->colorCodeConnectionFlow();
        if (wasOn != nowOn) {
            m_connFlowDelegate->setColorCodeEnabled(nowOn);
            if (m_connView) m_connView->viewport()->update();
        }
    }
    applyConnIfaceFilterToProxy();
    updateWindowTitle();

    // Push our desired cadence to the (possibly remote) backends. For local
    // backends this is a no-op; for the DBus proxies it sends a hint AND
    // doubles as a liveness signal that resets the agent's idle timer.
    const int desired = m_settings->pollIntervalMs();
    if (m_netMonitor)  m_netMonitor ->setDesiredIntervalMs(desired);
    if (m_connMonitor) m_connMonitor->setDesiredIntervalMs(desired);
    // (Re)arm the heartbeat at half the agent's hint TTL (10s) so the hint
    // never lapses between assertions. We use 4s to stay comfortably under
    // both the TTL and the agent's slow1 threshold (30s).
    if (!m_agentHeartbeat) {
        m_agentHeartbeat = new QTimer(this);
        m_agentHeartbeat->setTimerType(Qt::CoarseTimer);
        connect(m_agentHeartbeat, &QTimer::timeout, this, [this] {
            const int d = m_settings->pollIntervalMs();
            if (m_netMonitor)  m_netMonitor ->setDesiredIntervalMs(d);
            if (m_connMonitor) m_connMonitor->setDesiredIntervalMs(d);
        });
    }
    m_agentHeartbeat->start(4000);

    // Sub-poll display animation: when smoothing is on, repaint at a
    // tighter cadence than the data poll so smoothed rate changes ease
    // in/out visually between polls instead of stepping. Cadence is
    // max(100ms, pollMs/4). Stopped when smoothing is off.
    if (!m_smoothingTick) {
        m_smoothingTick = new QTimer(this);
        m_smoothingTick->setTimerType(Qt::CoarseTimer);
        connect(m_smoothingTick, &QTimer::timeout, this, [this] {
            if (m_connModel) m_connModel->advanceSmoothing();
        });
    }
    if (m_settings->rateSmoothingMs() > 0) {
        const int sub = std::max(100, desired / 4);
        if (!m_smoothingTick->isActive() || m_smoothingTick->interval() != sub)
            m_smoothingTick->start(sub);
    } else {
        m_smoothingTick->stop();
    }

    // Push the persisted filter expression into the line edit (if it
    // doesn't already match) so external Settings changes show up live.
    if (m_connFilterEdit) {
        const QString persisted = m_settings->connectionFilterExpr();
        if (m_connFilterEdit->text() != persisted) {
            QSignalBlocker block(m_connFilterEdit);
            m_connFilterEdit->setText(persisted);
        }
        applyConnFilterExpr();
    }
}

void MainWindow::selectConnectionsTab()
{
    if (m_tabs && m_connTab) m_tabs->setCurrentWidget(m_connTab);
}

void MainWindow::updateConnIfaceFilterVisibility()
{
    const bool onConn = (m_tabs && m_connTab &&
                         m_tabs->currentWidget() == m_connTab);
    if (m_connIfaceFilterToolbarAct)
        m_connIfaceFilterToolbarAct->setVisible(onConn);
    if (m_connFilterSepAct)
        m_connFilterSepAct->setVisible(onConn);
    if (m_connFilterSpacerAct)
        m_connFilterSpacerAct->setVisible(onConn);
    if (m_connFilterToolbarAct)
        m_connFilterToolbarAct->setVisible(onConn);
    if (m_connIfaceFilterMenuAct)
        m_connIfaceFilterMenuAct->setEnabled(onConn);
}

void MainWindow::onConnFilterTextChanged(const QString & /*text*/)
{
    // Debounce keystrokes so we don't reparse the expression on every
    // character — parse + reapply proxy filter is non-trivial on large
    // tables. 200ms is comfortable for typing speed without feeling laggy.
    if (!m_connFilterDebounce) {
        m_connFilterDebounce = new QTimer(this);
        m_connFilterDebounce->setSingleShot(true);
        connect(m_connFilterDebounce, &QTimer::timeout,
                this, &MainWindow::applyConnFilterExpr);
    }
    m_connFilterDebounce->start(200);
}

void MainWindow::applyConnFilterExpr()
{
    if (!m_connFilterEdit || !m_connProxy) return;
    const QString text = m_connFilterEdit->text();
    const QString err  = m_connProxy->setFilterExpression(text);

    // Visual feedback: red tint + tooltip with the parser error when the
    // expression doesn't parse. Otherwise show the syntax help on hover.
    QPalette pal = m_connFilterEdit->palette();
    if (!err.isEmpty()) {
        pal.setColor(QPalette::Base, QColor(0xff, 0xe5, 0xe5));
        m_connFilterEdit->setPalette(pal);
        m_connFilterEdit->setToolTip(tr("Filter error: %1").arg(err));
    } else {
        m_connFilterEdit->setPalette(QPalette{});
        m_connFilterEdit->setToolTip(tr("Filter expression — click ? for syntax"));
    }
    // Persist only valid expressions (or empty) — don't litter QSettings
    // with garbage that the user is mid-typing into.
    if (err.isEmpty() && m_settings)
        m_settings->setConnectionFilterExpr(text);
}

void MainWindow::showFilterHelp()
{
    // Build a real popup (Qt::Popup window flag) once and reuse. Unlike
    // QToolTip, a Qt::Popup widget stays open until the user clicks
    // outside it (or presses Escape) — no auto-dismiss timer to fight,
    // and the user can mouse over the contents to read them.
    if (!m_filterHelpPopup) {
        auto *frame = new QFrame(this, Qt::Popup);
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setFrameShadow(QFrame::Raised);
        frame->setAttribute(Qt::WA_DeleteOnClose, false);
        auto *lay = new QVBoxLayout(frame);
        lay->setContentsMargins(10, 8, 10, 8);
        auto *label = new QLabel(qiftop::filter::helpHtml(), frame);
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        label->setWordWrap(true);
        label->setMaximumWidth(560);
        lay->addWidget(label);
        m_filterHelpPopup = frame;
    }
    m_filterHelpPopup->adjustSize();
    // Anchor under the line edit's right edge so it doesn't cover what
    // the user is typing.
    if (m_connFilterEdit) {
        const QPoint anchor = m_connFilterEdit->mapToGlobal(
            QPoint(m_connFilterEdit->width() - m_filterHelpPopup->width(),
                   m_connFilterEdit->height() + 4));
        m_filterHelpPopup->move(anchor);
    }
    m_filterHelpPopup->show();
    m_filterHelpPopup->raise();
}

void MainWindow::applyConnIfaceFilterToProxy()
{
    const QStringList list = m_settings->connectionVisibleIfaces();
    QSet<QString> set(list.begin(), list.end());
    m_connProxy->setVisibleIfaces(set);

    if (m_connIfaceFilterBtn) {
        if (set.isEmpty())
            m_connIfaceFilterBtn->setText(tr("All interfaces"));
        else if (set.size() == 1)
            m_connIfaceFilterBtn->setText(*set.cbegin());
        else
            m_connIfaceFilterBtn->setText(tr("%n interface(s)", nullptr, set.size()));
    }

    // Iface filter is one of the things rendered into the title bar; keep
    // it in sync whenever the filter changes.
    updateWindowTitle();
}

void MainWindow::rebuildConnIfaceFilterMenu(const QList<InterfaceStats> &stats)
{
    if (!m_connIfaceFilterMenu) return;

    // Collect a stable, deduplicated list of names (ifname == empty is the
    // "unattributed" sentinel — represented as "—" in the menu).
    QStringList names;
    QSet<QString> seen;
    for (const auto &s : stats) {
        if (s.name.isEmpty() || seen.contains(s.name)) continue;
        seen.insert(s.name);
        names << s.name;
    }
    std::sort(names.begin(), names.end());

    const QStringList   visibleList = m_settings->connectionVisibleIfaces();
    const QSet<QString> visible(visibleList.begin(), visibleList.end());

    m_connIfaceFilterMenu->clear();

    auto *allAct = m_connIfaceFilterMenu->addAction(tr("All interfaces"));
    allAct->setCheckable(true);
    allAct->setChecked(visible.isEmpty());
    connect(allAct, &QAction::triggered, this, [this] {
        m_settings->setConnectionVisibleIfaces({});
    });
    m_connIfaceFilterMenu->addSeparator();

    auto addItem = [&](const QString &display, const QString &value) {
        auto *act = m_connIfaceFilterMenu->addAction(display);
        act->setCheckable(true);
        act->setChecked(!visible.isEmpty() && visible.contains(value));
        connect(act, &QAction::triggered, this, [this, value](bool checked) {
            QStringList cur = m_settings->connectionVisibleIfaces();
            QSet<QString> set(cur.begin(), cur.end());
            if (checked) set.insert(value);
            else         set.remove(value);
            // If the user just unchecked the last item, drop back to "all".
            cur = QStringList(set.cbegin(), set.cend());
            std::sort(cur.begin(), cur.end());
            m_settings->setConnectionVisibleIfaces(cur);
        });
    };

    for (const auto &n : names)
        addItem(n, n);
    m_connIfaceFilterMenu->addSeparator();
    addItem(tr("Unattributed (—)"), QString());
}

void MainWindow::onSettingsChanged()
{
    applySettingsToUi();
}

void MainWindow::onStatsUpdated(const QList<InterfaceStats> &stats)
{
    if (!m_paused)
        m_netModel->updateStats(stats);
    updateStatusBar(stats);
    if (m_tray)
        m_tray->onStatsUpdated(stats);
    rebuildConnIfaceFilterMenu(stats);

    // Feed the connections model the latest set of our own addresses so it
    // can collapse them to "localhost" when DNS resolution is on.
    QSet<QHostAddress> locals;
    for (const auto &s : stats) {
        for (const QString &cidr : s.addresses) {
            const QString ipPart = cidr.left(cidr.indexOf(QLatin1Char('/')));
            const QHostAddress a(ipPart);
            if (!a.isNull())
                locals.insert(a);
        }
    }
    m_connModel->setLocalAddresses(std::move(locals));
}

void MainWindow::onConnectionsUpdated(const QList<Connection> &conns)
{
    if (!m_paused)
        m_connModel->updateConnections(conns);
    m_statusConnections->setText(tr("%n connection(s)", nullptr,
                                    static_cast<int>(conns.size())));
}

void MainWindow::togglePaused(bool paused)
{
    m_paused = paused;
    m_pauseAction->setText(paused ? tr("Resume") : tr("Pause"));
    m_pauseAction->setToolTip(paused ? tr("Resume updates") : tr("Pause updates"));
    m_pauseAction->setIcon(QIcon::fromTheme(paused
        ? QStringLiteral("media-playback-start")
        : QStringLiteral("media-playback-pause")));
    if (m_tray)
        m_tray->setPaused(paused);
}

void MainWindow::openSettingsDialog()
{
    SettingsDialog dlg(m_settings, this);
    dlg.exec();
}

void MainWindow::runExport(Exportable *src, ExportFormat fmt,
                           ExportSink sink, const QString &baseName)
{
    const QByteArray data = (fmt == ExportFormat::Json)
        ? util::exporter::toJson(*src)
        : util::exporter::toCsv(*src);

    if (sink == ExportSink::Clipboard) {
        QGuiApplication::clipboard()->setText(QString::fromUtf8(data));
        statusBar()->showMessage(tr("Copied %1 %2 to clipboard")
                                     .arg(baseName,
                                          fmt == ExportFormat::Json ? tr("(JSON)")
                                                                    : tr("(CSV)")),
                                 5000);
        return;
    }

    const QString suffix = (fmt == ExportFormat::Json) ? QStringLiteral("json")
                                                       : QStringLiteral("csv");
    const QString filter = (fmt == ExportFormat::Json)
        ? tr("JSON files (*.json);;All files (*)")
        : tr("CSV files (*.csv);;All files (*)");
    const QString suggested = QStringLiteral("%1.%2").arg(baseName, suffix);

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export"), suggested, filter);
    if (path.isEmpty())
        return;

    QString err;
    if (!util::exporter::save(path, data, &err)) {
        QMessageBox::warning(this, tr("Export failed"),
                             tr("Could not write %1:\n%2").arg(path, err));
        return;
    }
    statusBar()->showMessage(tr("Exported to %1").arg(path), 5000);
}

// --- UI state persistence ----------------------------------------------------
//
// Window geometry, dock layout, current tab and per-table sort state are
// stored under "ui/..." in QSettings so they are independent of the user
// preferences exposed by the Preferences dialog.

namespace {
constexpr auto kUiGeometry         = "ui/geometry";
constexpr auto kUiWindowState      = "ui/windowState";
constexpr auto kUiCurrentTab       = "ui/currentTab";
constexpr auto kUiNetSortColumn    = "ui/interfaces/sortColumn";
constexpr auto kUiNetSortOrder     = "ui/interfaces/sortOrder";
constexpr auto kUiConnSortColumn   = "ui/connections/sortColumn";
constexpr auto kUiConnSortOrder    = "ui/connections/sortOrder";
} // namespace

void MainWindow::readUiState()
{
    QSettings s;

    if (const QByteArray geom = s.value(kUiGeometry).toByteArray(); !geom.isEmpty())
        restoreGeometry(geom);
    if (const QByteArray st = s.value(kUiWindowState).toByteArray(); !st.isEmpty())
        restoreState(st);

    m_tabs->setCurrentIndex(s.value(kUiCurrentTab, 0).toInt());

    const int netCol = s.value(kUiNetSortColumn,
                               static_cast<int>(NetworkModel::Column::Name)).toInt();
    const auto netOrder = static_cast<Qt::SortOrder>(
        s.value(kUiNetSortOrder, Qt::AscendingOrder).toInt());
    m_netView->sortByColumn(netCol, netOrder);

    const int connCol = s.value(kUiConnSortColumn,
                                static_cast<int>(ConnectionModel::Column::RxRate)).toInt();
    const auto connOrder = static_cast<Qt::SortOrder>(
        s.value(kUiConnSortOrder, Qt::DescendingOrder).toInt());
    m_connView->sortByColumn(connCol, connOrder);
}

void MainWindow::writeUiState()
{
    QSettings s;
    s.setValue(kUiGeometry,    saveGeometry());
    s.setValue(kUiWindowState, saveState());
    s.setValue(kUiCurrentTab,  m_tabs->currentIndex());

    s.setValue(kUiNetSortColumn, m_netView->horizontalHeader()->sortIndicatorSection());
    s.setValue(kUiNetSortOrder,
               static_cast<int>(m_netView->horizontalHeader()->sortIndicatorOrder()));
    s.setValue(kUiConnSortColumn, m_connView->horizontalHeader()->sortIndicatorSection());
    s.setValue(kUiConnSortOrder,
               static_cast<int>(m_connView->horizontalHeader()->sortIndicatorOrder()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Hide to tray instead of exiting when the user has opted in AND a tray
    // is actually available. Real quit goes through quitFromTray() / qApp->quit.
    if (!m_explicitQuit && m_settings->closeToTray()
        && m_tray && m_tray->isAvailable())
    {
        hide();
        event->ignore();
        return;
    }
    writeUiState();
    QMainWindow::closeEvent(event);
}

void MainWindow::quitFromTray()
{
    m_explicitQuit = true;
    qApp->quit();
}

void MainWindow::onConnectionsPermissionDenied(const QString &detail)
{
    if (!m_connBanner) return;
    m_connBannerLbl->setText(tr("Cannot enumerate connections — %1. "
                                "Per-flow data requires CAP_NET_ADMIN.")
                                 .arg(detail));
    m_connBanner->setVisible(true);
}

void MainWindow::onConnectionsAccountingUnavailable(const QString &detail)
{
    if (!m_connBanner) return;
    // Don't clobber a more serious EPERM banner that's already showing.
    if (m_connBanner->isVisible()) return;
    m_connBannerLbl->setText(tr("Per-flow byte/packet counters disabled — %1. "
                                "Flows will appear but rates and totals will read 0.")
                                 .arg(detail));
    m_connBanner->setVisible(true);
}

void MainWindow::relaunchAsAdmin()
{
    // Spin up the persistent IPC channel. On READY we'll stop our own data
    // collection and enter proxy mode (parent stays alive as the tray host).
    auto *handoff = new util::HandoffServer(this);
    const QString sockPath = handoff->listen();
    if (sockPath.isEmpty()) {
        qCWarning(lcVerbose).noquote()
            << "handoff: failed to start server:" << handoff->errorString();
        handoff->deleteLater();
        handoff = nullptr;
    } else {
        qputenv("QIFTOP_HANDOFF_SOCKET", sockPath.toLocal8Bit());
        qputenv("QIFTOP_HANDOFF_NONCE",  handoff->nonce().toLatin1());
        prepareProxyMode(handoff);
    }

    util::PrivilegeEscalator esc(this);
    esc.setVerbose(util::logging::isVerbose());
    connect(&esc, &util::PrivilegeEscalator::status, this, [this](const QString &s) {
        if (m_connBannerLbl)
            m_connBannerLbl->setText(s);
        statusBar()->showMessage(s, 5000);
    });

    if (util::logging::isVerbose()) {
        qCInfo(lcVerbose).noquote()
            << "privilege escalation plan:" << esc.plannedStrategies();
    }

    const QString self     = QCoreApplication::applicationFilePath();
    const QStringList tail = QCoreApplication::arguments().mid(1);

    QString used;
    const bool ok = esc.relaunch(self, tail, &used);
    qunsetenv("QIFTOP_HANDOFF_SOCKET");
    qunsetenv("QIFTOP_HANDOFF_NONCE");

    if (ok) {
        m_connBannerLbl->setText(tr("Privileged instance launching via %1. "
                                     "This window will minimise to the tray "
                                     "once it is ready.").arg(used));
    } else {
        if (handoff) handoff->deleteLater();
        QMessageBox::warning(this, tr("Relaunch failed"),
            tr("No graphical privilege helper succeeded "
               "(tried: %1).\n\nRestart qiftop manually with sudo, or grant "
               "the capability once with:\n\n    sudo setcap cap_net_admin+eip %2")
               .arg(esc.plannedStrategies().join(QStringLiteral(", ")), self));
    }
}

void MainWindow::prepareProxyMode(util::HandoffServer *server)
{
    // On READY: cut our local data collection, hide the window, route the
    // tray's user actions through the IPC channel instead of locally.
    connect(server, &util::HandoffServer::childReady, this, [this, server] {
        qCInfo(lcVerbose) << "proxy: entering proxy mode";
        m_proxyMode = true;

        // Stop hammering netlink/conntrack now that the child is doing it.
        if (m_netMonitor)  m_netMonitor->stop();
        if (m_connMonitor) m_connMonitor->stop();

        hide();

        // Replace local stats source with the IPC-relayed one.
        disconnect(m_netMonitor, &NetworkMonitor::statsUpdated, this, nullptr);
        connect(server, &util::HandoffServer::childStats, this,
                [this](const QList<InterfaceStats> &s) {
                    if (m_tray) m_tray->onStatsUpdated(s);
                });

        // Route tray menu actions to the privileged child.
        if (m_tray) {
            disconnect(m_tray, &TrayManager::showWindowRequested, this, nullptr);
            disconnect(m_tray, &TrayManager::pauseToggled,        nullptr, nullptr);
            disconnect(m_tray, &TrayManager::quitRequested,       this, nullptr);

            connect(m_tray, &TrayManager::showWindowRequested,
                    server, &util::HandoffServer::sendShow);
            connect(m_tray, &TrayManager::pauseToggled,
                    server, &util::HandoffServer::sendPause);
            connect(m_tray, &TrayManager::quitRequested,
                    server, &util::HandoffServer::sendQuit);

            // Reflect the child's pause state back into the tray check item.
            connect(server, &util::HandoffServer::childPauseState,
                    m_tray, &TrayManager::setPaused);
        }
    });

    // If the child dies (clean BYE or crash), follow it down.
    connect(server, &util::HandoffServer::childDisconnected, this, [this] {
        if (m_proxyMode) {
            qCInfo(lcVerbose) << "proxy: child gone — quitting";
            m_explicitQuit = true;
            qApp->quit();
        }
    });
}

void MainWindow::setBackendInfo(bool usingAgent,
                                const QString     &version,
                                const QStringList &caps)
{
    if (!m_statusBackend) return;
    if (usingAgent) {
        const QString shown = version.isEmpty() ? tr("(legacy)") : version;
        m_statusBackend->setText(tr("agent %1").arg(shown));
        const QString tip = caps.isEmpty()
            ? tr("Connected to qiftop-agent over DBus. No capability tokens reported.")
            : tr("Connected to qiftop-agent over DBus.\nCapabilities: %1")
                  .arg(caps.join(QStringLiteral(", ")));
        m_statusBackend->setToolTip(tip);
    } else {
        m_statusBackend->setText(tr("in-process"));
        m_statusBackend->setToolTip(tr("qiftop-agent unavailable; using the "
                                       "in-process Netlink/conntrack backend. "
                                       "Some flows may be hidden without CAP_NET_ADMIN."));
    }
    // Reset any cadence-degradation tint left over from a previous state.
    m_statusBackend->setStyleSheet(QString());
}

void MainWindow::notifyAgentCadence(int intervalMs)
{
    if (!m_statusBackend) return;
    // Tint the backend label so the user immediately notices when the agent
    // has slowed itself down (idle-manager wind-down) or paused entirely.
    // We treat anything significantly above 1 s as "slowed", and ms==0 as
    // "paused". The exact thresholds match the agent's defaults but the UI
    // doesn't depend on them — we just colour-code three buckets.
    QString css;
    QString suffix;
    if (intervalMs <= 0) {
        css    = QStringLiteral("color: palette(highlightedText); background: #b00020;");
        suffix = tr(" — paused");
    } else if (intervalMs > 1500) {
        css    = QStringLiteral("color: palette(windowText); background: #c69026;");
        suffix = tr(" — slowed (%1 ms)").arg(intervalMs);
    } else {
        css    = QString(); // back to default appearance
        suffix.clear();
    }
    m_statusBackend->setStyleSheet(css);
    // Preserve the agent-version text we set in setBackendInfo by reading
    // it back and stripping any previous suffix, then re-appending.
    QString text = m_statusBackend->text();
    const int dash = text.indexOf(QStringLiteral(" — "));
    if (dash > 0) text.truncate(dash);
    m_statusBackend->setText(text + suffix);
}

void MainWindow::attachHandoffClient(util::HandoffClient *client)
{
    if (!client) return;

    // Mirror every stats tick to the parent so its tray stays live.
    connect(m_netMonitor, &NetworkMonitor::statsUpdated,
            client,       &util::HandoffClient::sendStats);

    // Echo our pause state up.
    connect(m_pauseAction, &QAction::toggled,
            client,        &util::HandoffClient::sendPauseState);

    // Commands flowing down from the parent's tray.
    connect(client, &util::HandoffClient::showRequested, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });
    connect(client, &util::HandoffClient::pauseCommand,
            m_pauseAction, &QAction::setChecked);
    connect(client, &util::HandoffClient::quitCommand,
            this,         &MainWindow::quitFromTray);
}

void MainWindow::updateWindowTitle()
{
    const QString base = tr("qiftop — Network Monitor");
    if (!m_settings || !m_settings->showStatusInTitle()) {
        setWindowTitle(base);
        return;
    }

    QStringList parts;

    // Poll cadence — render compactly (e.g. "1s", "500ms", "1.5s").
    const int ms = m_settings->pollIntervalMs();
    if (ms < 1000)
        parts << tr("%1ms").arg(ms);
    else if (ms % 1000 == 0)
        parts << tr("%1s").arg(ms / 1000);
    else
        parts << tr("%1s").arg(ms / 1000.0, 0, 'f', 1);

    // Interface filter — only when non-empty (i.e. not "all interfaces").
    // The empty-string sentinel "" (unattributed) is rendered as "—" to
    // match how it appears in the filter menu.
    const QStringList ifaces = m_settings->connectionVisibleIfaces();
    if (!ifaces.isEmpty()) {
        QStringList shown = ifaces;
        for (QString &s : shown)
            if (s.isEmpty()) s = QStringLiteral("—");
        parts << shown.join(QLatin1Char(','));
    }

    // Protocol-family toggles. Only mention when not the default
    // (both on); "(no protocols)" if the user has turned both off.
    const bool tcp = m_settings->showTcp();
    const bool udp = m_settings->showUdp();
    if (!tcp && !udp)        parts << tr("no protocols");
    else if (tcp && !udp)    parts << QStringLiteral("TCP");
    else if (!tcp && udp)    parts << QStringLiteral("UDP");
    // both on = default → omit

    // IPv6 visibility — only call out when off (default is on).
    if (!m_settings->ipv6Enabled())
        parts << QStringLiteral("IPv4-only");

    // Stale-row retention off (both UDP and others) is worth surfacing
    // since rows then vanish immediately on close.
    if (m_settings->connectionStaleRetentionSecs() == 0
        && m_settings->connectionStaleRetentionSecsUdp() == 0)
    {
        parts << tr("no linger");
    }

    if (parts.isEmpty()) {
        setWindowTitle(base);
    } else {
        // Use middle-dot separators so the suffix reads as a single
        // grouped status field rather than competing with the em-dash
        // already in the base title.
        setWindowTitle(QStringLiteral("%1 — %2")
                           .arg(base, parts.join(QStringLiteral(" · "))));
    }
}

void MainWindow::appendViewToggleSection(QMenu *menu)
{
    if (!menu)
        return;
    if (!menu->isEmpty())
        menu->addSeparator();

    // Menu bar toggle. QMenuBar has no built-in toggleViewAction(), so we
    // wire one up by hand and keep its checked state in sync with the
    // actual visibility on each popup.
    QMenuBar *mb = menuBar();
    if (mb) {
        QAction *toggleMenuBar = menu->addAction(tr("Show Menu Bar"));
        toggleMenuBar->setCheckable(true);
        toggleMenuBar->setChecked(mb->isVisible());
        connect(toggleMenuBar, &QAction::toggled, this, [this](bool on) {
            if (QMenuBar *bar = menuBar())
                bar->setVisible(on);
        });
    }

    // Toolbar already provides a checkable toggleViewAction() that mirrors
    // its visibility; reuse it so state stays consistent with the View menu.
    if (m_toolbar) {
        QAction *tbAct = m_toolbar->toggleViewAction();
        tbAct->setText(tr("Show Toolbar"));
        menu->addAction(tbAct);
    }

    menu->addSeparator();
    if (m_settingsAction)
        menu->addAction(m_settingsAction);
    else
        menu->addAction(tr("Preferences…"), this, &MainWindow::openSettingsDialog);
}

void MainWindow::showInterfaceContextMenu(const QPoint &pos)
{
    const QModelIndex idx = m_netView->indexAt(pos);

    QMenu menu(this);

    if (idx.isValid()) {
        // Resolve the ifname from the Name column regardless of which column was clicked.
        const QModelIndex nameIdx = idx.sibling(idx.row(),
                                                static_cast<int>(NetworkModel::Column::Name));
        const QString ifname = nameIdx.data(Qt::DisplayRole).toString();
        if (!ifname.isEmpty()) {
            QStringList summary = m_settings->trayInterfaces();
            const bool inSummary = summary.contains(ifname);

            QAction *toggleAct = menu.addAction(inSummary
                ? tr("Remove “%1” from tray summary").arg(ifname)
                : tr("Add “%1” to tray summary").arg(ifname));
            if (!m_tray || !m_tray->isAvailable())
                toggleAct->setEnabled(false);

            connect(toggleAct, &QAction::triggered, this, [this, ifname, inSummary] {
                QStringList list = m_settings->trayInterfaces();
                if (inSummary) list.removeAll(ifname);
                else if (!list.contains(ifname)) list.append(ifname);
                m_settings->setTrayInterfaces(list);
            });
        }
    }

    appendViewToggleSection(&menu);
    menu.exec(m_netView->viewport()->mapToGlobal(pos));
}

void MainWindow::showConnectionContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    const QModelIndex viewIdx = m_connView->indexAt(pos);
    if (viewIdx.isValid()) {
        // Map through the filter proxy to a source row before asking the model
        // for its copy text — proxy row indices are unstable under sort/filter.
        const QModelIndex srcIdx = m_connProxy->mapToSource(viewIdx);
        if (srcIdx.isValid()) {
            const int row = srcIdx.row();

            const QString srcText  = m_connModel->copyTextForEndpoint(
                row, ConnectionModel::FlowEnd::Source);
            const QString dstText  = m_connModel->copyTextForEndpoint(
                row, ConnectionModel::FlowEnd::Destination);
            const QString lineText = m_connModel->copyTextForFlow(row);

            auto *copySrc  = menu.addAction(tr("Copy source: %1").arg(srcText));
            auto *copyDst  = menu.addAction(tr("Copy destination: %1").arg(dstText));
            menu.addSeparator();
            auto *copyLine = menu.addAction(tr("Copy entire connection line"));

            const auto copyToClip = [](const QString &s) {
                QApplication::clipboard()->setText(s);
            };
            connect(copySrc,  &QAction::triggered, this, [copyToClip, srcText]  { copyToClip(srcText);  });
            connect(copyDst,  &QAction::triggered, this, [copyToClip, dstText]  { copyToClip(dstText);  });
            connect(copyLine, &QAction::triggered, this, [copyToClip, lineText] { copyToClip(lineText); });
        }
    }

    appendViewToggleSection(&menu);
    menu.exec(m_connView->viewport()->mapToGlobal(pos));
}

void MainWindow::updateStatusBar(const QList<InterfaceStats> &stats)
{
    quint64 totalRx = 0;
    quint64 totalTx = 0;
    int count = 0;
    for (const auto &s : stats) {
        if (s.isLoopback)
            continue;
        ++count;
        totalRx += s.rxBytes;
        totalTx += s.txBytes;
    }
    m_statusInterfaces->setText(tr("%n interface(s)", nullptr, count));
    m_statusThroughput->setText(QStringLiteral("↓ %1   ↑ %2")
                                    .arg(util::formatBytes(totalRx),
                                         util::formatBytes(totalTx)));
}
