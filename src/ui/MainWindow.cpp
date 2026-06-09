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
#include <QShortcut>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTreeView>
#include <QComboBox>

#include "ui/ConnectionGroupProxy.h"
#include "ui/ConnectionAttributionDelegate.h"
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
    installShortcuts();

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

    // Right-click on the interfaces table header → checkable per-column
    // visibility toggles. Visual order + width persistence already lives
    // in saveHeaderState(); this menu only manages hidden-ness on top.
    m_netView->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_netView->horizontalHeader(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showNetHeaderMenu);
    // Allow drag-reordering of columns (state captured by saveState()).
    m_netView->horizontalHeader()->setSectionsMovable(true);

    // --- Connections tab ---
    m_connModel = new ConnectionModel(this);
    m_connModel->setDnsResolver(m_dnsResolver);

    m_connProxy = new ConnectionFilterProxy(this);
    m_connProxy->setSourceModel(m_connModel);
    m_connProxy->setSortRole(ConnectionModel::SortRole);

    // Group proxy: pass-through in Flat mode (default), tree-of-groups
    // in the by-X modes. Sits BETWEEN the filter proxy and the view so
    // grouping always sees post-filter rows. Mode is applied in
    // applySettingsToUi() based on Settings::connectionViewMode().
    m_connGroupProxy = new ConnectionGroupProxy(this);
    m_connGroupProxy->setSourceModel(m_connProxy);

    m_connView = new QTreeView;
    m_connView->setModel(m_connGroupProxy);
    m_connView->setSortingEnabled(true);
    m_connView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connView->setAlternatingRowColors(true);
    m_connView->setUniformRowHeights(true);
    // Flat-mode defaults — match v0.1's QTableView geometry exactly so
    // the RowGaugeDelegate / ConnectionFlowDelegate keep painting at
    // the same coordinates. The mode-switch path in applySettingsToUi()
    // toggles these for the grouped modes.
    m_connView->setRootIsDecorated(false);
    m_connView->setItemsExpandable(false);
    m_connView->setIndentation(0);
    m_connView->setExpandsOnDoubleClick(false);
    m_connView->header()->setStretchLastSection(false);
    m_connView->header()->setSectionResizeMode(
        static_cast<int>(ConnectionModel::Column::Flow), QHeaderView::Stretch);
    m_connFlowDelegate = new ConnectionFlowDelegate(m_connView);
    m_connView->setItemDelegateForColumn(
        static_cast<int>(ConnectionModel::Column::Flow),
        m_connFlowDelegate);
    // Default delegate for the other columns: paints the row-spanning
    // throughput gauge background before chaining to the standard styled
    // item rendering. Owned by the view (parent).
    m_connView->setItemDelegate(new RowGaugeDelegate(m_connView, m_connView));
    // Process + Container columns get a richer delegate that paints
    // a small grey pid badge / chain-depth chevron next to the
    // primary text. It inherits from RowGaugeDelegate so the
    // throughput gauge still works underneath.
    auto *attribDelegate = new ConnectionAttributionDelegate(m_connView, m_connView);
    m_connView->setItemDelegateForColumn(
        static_cast<int>(ConnectionModel::Column::Process), attribDelegate);
    m_connView->setItemDelegateForColumn(
        static_cast<int>(ConnectionModel::Column::Container), attribDelegate);
    // Max columns are only meaningful with the gauge enabled; hide by
    // default and (un)hide in applySettingsToUi() based on the setting.
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::RxMax), true);
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::TxMax), true);
    // Attribution columns are hidden by default — they only make sense
    // when the agent advertises the matching wire-attribution tokens,
    // and the Settings > Display sub-section (s5-settings) lets the
    // user enable them. Until then they're available through the
    // header right-click menu.
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::Process), true);
    m_connView->setColumnHidden(
        static_cast<int>(ConnectionModel::Column::Container), true);
    m_connView->sortByColumn(static_cast<int>(ConnectionModel::Column::RxRate),
                             Qt::DescendingOrder);
    m_connView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connView, &QWidget::customContextMenuRequested,
            this,        &MainWindow::showConnectionContextMenu);

    // Right-click on the connections table header → per-column visibility
    // toggles. Same model as the interfaces header (above).
    m_connView->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_connView->header(), &QWidget::customContextMenuRequested,
            this, &MainWindow::showConnHeaderMenu);
    m_connView->header()->setSectionsMovable(true);

    // Empty-state placeholder: a centered hint shown when the model has
    // zero rows. Parented to the viewport so it scrolls with the (empty)
    // canvas and inherits the table's palette. Visibility is driven by
    // onConnectionsUpdated() and the proxy's rowsInserted/rowsRemoved.
    m_connEmptyOverlay = new QLabel(m_connView->viewport());
    m_connEmptyOverlay->setAlignment(Qt::AlignCenter);
    m_connEmptyOverlay->setWordWrap(true);
    m_connEmptyOverlay->setText(tr(
        "<p style='color: palette(placeholderText); font-size: large;'>"
        "<b>No active flows</b></p>"
        "<p style='color: palette(placeholderText);'>"
        "Generate some traffic — for example "
        "<code>ping 1.1.1.1</code> or open a web page — and rows will "
        "appear here.</p>"
        "<p style='color: palette(placeholderText);'>"
        "If you expected flows but see none, check the filter expression "
        "(Ctrl+F) and the active protocol / interface filters.</p>"));
    m_connEmptyOverlay->setTextFormat(Qt::RichText);
    m_connEmptyOverlay->setContextMenuPolicy(Qt::NoContextMenu);
    m_connEmptyOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_connEmptyOverlay->hide();
    // Keep the overlay sized to the viewport. We install a single
    // eventFilter on the viewport (Show/Resize) and re-layout in the
    // handler. installEventFilter is set later, after m_connView is
    // fully constructed.
    m_connView->viewport()->installEventFilter(this);
    // Toggle visibility whenever the proxy gains/loses rows. Using the
    // proxy (not the source model) is intentional: a filter that hides
    // every row should also show the empty-state, with copy adjusted
    // by the existing text (which already mentions "check the filter").
    auto refreshEmpty = [this] {
        if (!m_connEmptyOverlay || !m_connProxy) return;
        const bool empty = m_connProxy->rowCount() == 0;
        m_connEmptyOverlay->setVisible(empty);
        if (empty) {
            m_connEmptyOverlay->resize(m_connView->viewport()->size());
            m_connEmptyOverlay->move(0, 0);
        }
    };
    connect(m_connProxy, &QAbstractItemModel::rowsInserted,       this, refreshEmpty);
    connect(m_connProxy, &QAbstractItemModel::rowsRemoved,        this, refreshEmpty);
    connect(m_connProxy, &QAbstractItemModel::modelReset,         this, refreshEmpty);
    connect(m_connProxy, &QAbstractItemModel::layoutChanged,      this, refreshEmpty);
    refreshEmpty();

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

    // "View as" dropdown (Flat / by Interface / by Container / by
    // Process). Same visibility rules as the iface filter button —
    // only the Connections tab cares — wired up via
    // updateConnIfaceFilterVisibility() below.
    auto *viewModeBox    = new QWidget(toolbar);
    viewModeBox->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *viewModeLayout = new QHBoxLayout(viewModeBox);
    viewModeLayout->setContentsMargins(6, 0, 0, 0);
    viewModeLayout->setSpacing(6);
    auto *viewModeLabel  = new QLabel(tr("View:"), viewModeBox);
    viewModeLabel->setForegroundRole(QPalette::WindowText);
    m_connViewModeCombo  = new QComboBox(viewModeBox);
    m_connViewModeCombo->addItem(tr("Flat"),
                                 static_cast<int>(Settings::ConnectionViewMode::Flat));
    m_connViewModeCombo->addItem(tr("by Interface"),
                                 static_cast<int>(Settings::ConnectionViewMode::ByInterface));
    m_connViewModeCombo->addItem(tr("by Container"),
                                 static_cast<int>(Settings::ConnectionViewMode::ByContainer));
    m_connViewModeCombo->addItem(tr("by Process"),
                                 static_cast<int>(Settings::ConnectionViewMode::ByProcess));
    m_connViewModeCombo->setToolTip(tr(
        "Group connections. \"Flat\" matches the classic iftop view."));
    m_connViewModeCombo->setCurrentIndex(
        static_cast<int>(m_settings->connectionViewMode()));
    connect(m_connViewModeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int i) {
                if (i < 0) return;
                m_settings->setConnectionViewMode(
                    static_cast<Settings::ConnectionViewMode>(i));
            });
    viewModeLayout->addWidget(viewModeLabel);
    viewModeLayout->addWidget(m_connViewModeCombo);
    m_connViewModeToolbarAct = toolbar->addWidget(viewModeBox);

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

    // --- Help menu: About qiftop + About Qt -----------------------------
    // Kept minimal on purpose; "About qiftop" pops a dialog that shows
    // both client and (when reachable) agent metadata so users can
    // include exact version info in bug reports without grepping the
    // process tree.
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    auto *shortcutsAct = helpMenu->addAction(
        tr("&Keyboard Shortcuts…"), this, &MainWindow::showShortcutsDialog);
    shortcutsAct->setShortcut(QKeySequence::HelpContents); // F1
    shortcutsAct->setShortcutContext(Qt::ApplicationShortcut);
    helpMenu->addSeparator();
    helpMenu->addAction(
        QIcon::fromTheme(QStringLiteral("help-about")),
        tr("&About qiftop…"), this, &MainWindow::showAboutDialog);
    helpMenu->addAction(tr("About &Qt…"), qApp, &QApplication::aboutQt);
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
        // Attribution columns: visibility driven by Settings, but only
        // when the connected agent actually carries the matching wire
        // tokens. Without them the columns would just render "—" /
        // "(host)" everywhere, which is misleading rather than helpful.
        const bool procWire = m_agentCaps.contains(
            QStringLiteral("process-attribution-wire"));
        const bool contWire = m_agentCaps.contains(
            QStringLiteral("container-attribution-wire"));
        m_connView->setColumnHidden(
            static_cast<int>(ConnectionModel::Column::Process),
            !(procWire && m_settings->showProcessColumn()));
        m_connView->setColumnHidden(
            static_cast<int>(ConnectionModel::Column::Container),
            !(contWire && m_settings->showContainerColumn()));
        m_connModel->setShowContainerChainInTooltip(
            m_settings->showContainerChainInTooltip());
    }
    if (m_connView && m_connGroupProxy) {
        // Apply the persisted view mode to the group proxy and adjust
        // the QTreeView decorations so Flat mode stays pixel-identical
        // to v0.1 (no indent, no branch markers) and grouped modes
        // expose the normal tree branches.
        const auto mode = m_settings->connectionViewMode();
        m_connGroupProxy->setViewMode(mode);
        const bool flat = (mode == Settings::ConnectionViewMode::Flat);
        m_connView->setRootIsDecorated(!flat);
        m_connView->setItemsExpandable(!flat);
        m_connView->setIndentation(flat ? 0 : 14);
        m_connView->setExpandsOnDoubleClick(!flat);
        if (!flat) m_connView->expandAll();
        // Keep the view-mode dropdown in the toolbar in sync if a
        // change came in via the Settings dialog rather than the
        // dropdown itself.
        if (m_connViewModeCombo
            && m_connViewModeCombo->currentIndex() != static_cast<int>(mode)) {
            const QSignalBlocker block(m_connViewModeCombo);
            m_connViewModeCombo->setCurrentIndex(static_cast<int>(mode));
        }
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
    if (m_connViewModeToolbarAct)
        m_connViewModeToolbarAct->setVisible(onConn);
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

void MainWindow::showAboutDialog()
{
    // QMessageBox::about() renders rich text and gives us a focusable
    // dialog with a Copy-to-clipboard affordance via right-click — much
    // friendlier for bug reports than a plain "About" popup. We hand-roll
    // the body so we can mention the agent contract version & active
    // capability tokens (this is the one place where that information is
    // actually useful to surface, beyond the status-bar tooltip).
    const QString appVer = QCoreApplication::applicationVersion();
    const QString qtVer  = QString::fromLatin1(qVersion());

    QString backendLine;
    if (m_usingAgent) {
        const QString agentShown = m_agentVersion.isEmpty()
                                       ? tr("(legacy, pre-Version)")
                                       : m_agentVersion;
        backendLine = tr("Backend: <b>qiftop-agent %1</b> over DBus")
                          .arg(agentShown.toHtmlEscaped());
    } else {
        backendLine = tr("Backend: <b>in-process</b> "
                         "(Netlink / conntrack, self-elevated)");
    }

    QString capsLine;
    if (m_usingAgent && !m_agentCaps.isEmpty()) {
        // Capability tokens are stable identifiers; render in monospace so
        // they're easy to copy/paste into bug reports.
        QStringList esc;
        esc.reserve(m_agentCaps.size());
        for (const QString &t : m_agentCaps)
            esc << t.toHtmlEscaped();
        capsLine = tr("<p>Agent capabilities:<br>"
                      "<code style=\"font-size:small\">%1</code></p>")
                       .arg(esc.join(QStringLiteral(", ")));
    }

    const QString body = tr(
        "<h3>qiftop %1</h3>"
        "<p>An iftop-style network monitor for Qt 6 on Linux.</p>"
        "<p>%2<br>"
        "Qt runtime: %3</p>"
        "%4"
        "<p><a href=\"https://github.com/TheCleaners/qiftop\">"
        "github.com/TheCleaners/qiftop</a></p>"
        "<p style=\"color:gray;font-size:small\">"
        "Licensed under the GNU General Public License v2 or later."
        "</p>")
        .arg(appVer.toHtmlEscaped(),
             backendLine,
             qtVer.toHtmlEscaped(),
             capsLine);

    QMessageBox::about(this, tr("About qiftop"), body);
}

void MainWindow::showShortcutsDialog()
{
    // Static-content reference dialog. Anything bound via installShortcuts()
    // or via QAction::setShortcut() in createMenusAndToolbar() should be
    // listed here — there's no way to discover the global QShortcut-based
    // bindings (Ctrl+F, Esc, Ctrl+C in tables, Ctrl+N tab switch) through
    // the menu UI, so this dialog is their canonical home.
    const QString body = tr(
        "<h3>Keyboard Shortcuts</h3>"
        "<table cellpadding=4>"
        "<tr><th align=left colspan=2>Navigation</th></tr>"
        "<tr><td><b>Ctrl+1</b> … <b>Ctrl+9</b></td>"
        "<td>Switch to tab <i>N</i> (Interfaces / Connections)</td></tr>"
        "<tr><td><b>F1</b></td><td>Show this dialog</td></tr>"
        "<tr><td><b>Ctrl+,</b></td><td>Open Preferences</td></tr>"
        "<tr><td><b>Ctrl+W</b></td><td>Close window (stays in tray if enabled)</td></tr>"
        "<tr><td><b>Ctrl+Q</b></td><td>Quit the application</td></tr>"
        "<tr><th align=left colspan=2>Filtering (Connections tab)</th></tr>"
        "<tr><td><b>Ctrl+F</b></td>"
        "<td>Focus the filter expression bar (switches tab if needed)</td></tr>"
        "<tr><td><b>Esc</b></td>"
        "<td>Clear the filter expression (when filter bar has focus)</td></tr>"
        "<tr><th align=left colspan=2>Selection</th></tr>"
        "<tr><td><b>Ctrl+C</b></td>"
        "<td>Copy selected rows to clipboard "
        "(connections as flow lines, interfaces as TSV)</td></tr>"
        "<tr><th align=left colspan=2>Context menu (right-click a row)</th></tr>"
        "<tr><td>Connections</td>"
        "<td>Copy source / destination / line · "
        "Show / Hide flows to/from peer · Resolve hostname</td></tr>"
        "<tr><td>Interfaces</td>"
        "<td>Copy interface name · Reset counters</td></tr>"
        "<tr><th align=left colspan=2>Filter mini-language</th></tr>"
        "<tr><td colspan=2>The filter bar accepts a small expression "
        "language — click the <b>?</b> button next to it for the full "
        "syntax (fields like <tt>host</tt>, <tt>proto</tt>, "
        "<tt>rate_total</tt>; operators <tt>:</tt> <tt>=</tt> <tt>~</tt> "
        "<tt>&lt;</tt> <tt>&gt;</tt>; combinators <tt>and</tt> / "
        "<tt>or</tt> / <tt>not</tt>).</td></tr>"
        "</table>");

    QMessageBox box(QMessageBox::NoIcon, tr("Keyboard Shortcuts"), body,
                    QMessageBox::Close, this);
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.exec();
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
    SettingsDialog dlg(m_settings, m_agentCaps, this);
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
constexpr auto kUiNetHeaderState   = "ui/interfaces/headerState";
constexpr auto kUiConnHeaderState  = "ui/connections/headerState";
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

    // Per-header column widths / order / hidden state. Restoring here
    // (after sortByColumn so QHeaderView::saveState's serialised sort
    // section index is the one that wins on next save) — Qt's
    // restoreState also pulls in the sort indicator state.
    if (const QByteArray nh = s.value(kUiNetHeaderState).toByteArray(); !nh.isEmpty())
        m_netView->horizontalHeader()->restoreState(nh);
    if (const QByteArray ch = s.value(kUiConnHeaderState).toByteArray(); !ch.isEmpty())
        m_connView->header()->restoreState(ch);
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
    s.setValue(kUiConnSortColumn, m_connView->header()->sortIndicatorSection());
    s.setValue(kUiConnSortOrder,
               static_cast<int>(m_connView->header()->sortIndicatorOrder()));

    s.setValue(kUiNetHeaderState,  m_netView->horizontalHeader()->saveState());
    s.setValue(kUiConnHeaderState, m_connView->header()->saveState());
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Resize the empty-state overlay together with the Connections viewport
    // so the placeholder stays centered and clipped to the visible area.
    if (m_connEmptyOverlay && m_connView
        && watched == m_connView->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show))
    {
        if (m_connEmptyOverlay->isVisible())
            m_connEmptyOverlay->resize(m_connView->viewport()->size());
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::installShortcuts()
{
    // Ctrl+F focuses the filter expression bar (and selects its contents so
    // the user can start typing immediately). Only meaningful on the
    // Connections tab; on the Interfaces tab we still focus it after
    // switching tabs — surprise-free for muscle memory.
    auto *focusFilter = new QShortcut(QKeySequence(QKeySequence::Find), this);
    focusFilter->setContext(Qt::ApplicationShortcut);
    connect(focusFilter, &QShortcut::activated, this, [this] {
        if (!m_connFilterEdit) return;
        if (m_tabs && m_tabs->currentWidget() != m_connTab && m_connTab)
            m_tabs->setCurrentWidget(m_connTab);
        m_connFilterEdit->setFocus(Qt::ShortcutFocusReason);
        m_connFilterEdit->selectAll();
    });

    // Esc: clears the filter when the filter bar has focus. Doesn't steal
    // global Esc (which closes dialogs, popups, etc.) — we install it on
    // the QLineEdit itself with WidgetShortcut context.
    if (m_connFilterEdit) {
        auto *clearFilter = new QShortcut(QKeySequence(Qt::Key_Escape), m_connFilterEdit);
        clearFilter->setContext(Qt::WidgetShortcut);
        connect(clearFilter, &QShortcut::activated, this, [this] {
            m_connFilterEdit->clear();
        });
    }

    // Ctrl+C in either table copies the selected rows. The view's default
    // Ctrl+C copies a single cell — we want the whole row(s), formatted via
    // the existing copyTextForFlow() / Exportable CSV emitter.
    auto installCopy = [this](QAbstractItemView *view) {
        auto *act = new QShortcut(QKeySequence(QKeySequence::Copy), view);
        act->setContext(Qt::WidgetShortcut);
        connect(act, &QShortcut::activated, this,
                [this, view] { copyTableSelectionToClipboard(view); });
    };
    if (m_connView) installCopy(m_connView);
    if (m_netView)  installCopy(m_netView);

    // Ctrl+1 / Ctrl+2: switch tabs. Common Qt-app convention; harmless on
    // single-tab platforms.
    if (m_tabs) {
        for (int i = 0; i < qMin(9, m_tabs->count()); ++i) {
            auto *sc = new QShortcut(
                QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
            sc->setContext(Qt::ApplicationShortcut);
            connect(sc, &QShortcut::activated, this, [this, i] {
                if (m_tabs && i < m_tabs->count())
                    m_tabs->setCurrentIndex(i);
            });
        }
    }
}

void MainWindow::copyTableSelectionToClipboard(QAbstractItemView *view)
{
    if (!view) return;
    auto *sel = view->selectionModel();
    if (!sel) return;
    const QModelIndexList rows = sel->selectedRows();
    if (rows.isEmpty()) return;

    QStringList lines;
    lines.reserve(rows.size());
    for (const QModelIndex &viewIdx : rows) {
        QModelIndex src = viewIdx;
        // Map through the (possibly chained) proxies down to the source
        // row so the model can use its own row indices. For the
        // Connections view this is GroupProxy → FilterProxy →
        // ConnectionModel; for the Interfaces view it's a single filter
        // proxy.
        if (view == m_connView) {
            if (m_connGroupProxy && m_connGroupProxy->isGroupIndex(viewIdx))
                continue;  // skip group headers; no flow to copy
            if (m_connGroupProxy)
                src = m_connGroupProxy->mapToSource(viewIdx);
            if (src.isValid() && m_connProxy)
                src = m_connProxy->mapToSource(src);
        } else if (auto *proxy = qobject_cast<QSortFilterProxyModel *>(view->model())) {
            src = proxy->mapToSource(viewIdx);
        }
        if (!src.isValid()) continue;
        if (view == m_connView && m_connModel) {
            lines << m_connModel->copyTextForFlow(src.row());
        } else if (view == m_netView && m_netModel) {
            QStringList cells;
            const int cols = m_netModel->columnCount();
            for (int c = 0; c < cols; ++c) {
                const QModelIndex idx = m_netModel->index(src.row(), c);
                cells << idx.data(Qt::DisplayRole).toString();
            }
            lines << cells.join(QLatin1Char('\t'));
        }
    }
    if (!lines.isEmpty())
        QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void MainWindow::filterByConnectionRow(const QPoint &pos, bool exclude)
{
    if (!m_connView || !m_connFilterEdit || !m_connModel || !m_connProxy) return;
    const QModelIndex viewIdx = m_connView->indexAt(pos);
    if (!viewIdx.isValid()) return;
    // viewIdx may be a group row (no source flow); skip it.
    if (m_connGroupProxy && m_connGroupProxy->isGroupIndex(viewIdx)) return;
    const QModelIndex filterIdx = m_connGroupProxy
                                      ? m_connGroupProxy->mapToSource(viewIdx)
                                      : viewIdx;
    if (!filterIdx.isValid()) return;
    const QModelIndex srcIdx = m_connProxy->mapToSource(filterIdx);
    if (!srcIdx.isValid()) return;

    const QString peer = m_connModel->peerAddressText(srcIdx.row());
    if (peer.isEmpty()) return;

    // Quote the address so IPv6 colons can't be mis-tokenised by the
    // filter parser. `host` matches both endpoints' addresses and (when
    // resolution is enabled) hostnames; for excluding it's the same field
    // wrapped in `not`.
    const QString clause = exclude
        ? QStringLiteral("not host=\"%1\"").arg(peer)
        : QStringLiteral("host=\"%1\"").arg(peer);
    m_connFilterEdit->setText(clause);
    m_connFilterEdit->setFocus(Qt::OtherFocusReason);
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
        // Pass the nonce via a 0600 file path on the env, not argv: the
        // nonce-on-argv form was world-readable via /proc/<pid>/cmdline
        // for the lifetime of the pkexec auth prompt.
        qputenv("QIFTOP_HANDOFF_NONCE_FILE", handoff->nonceFilePath().toLocal8Bit());
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
    qunsetenv("QIFTOP_HANDOFF_NONCE_FILE");

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
    m_usingAgent   = usingAgent;
    m_agentVersion = version;
    m_agentCaps    = caps;
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
    if (viewIdx.isValid() && !(m_connGroupProxy && m_connGroupProxy->isGroupIndex(viewIdx))) {
        // Map view → group proxy → filter proxy → source. Proxy row indices
        // are unstable under sort/filter, so we map all the way down before
        // asking the model.
        const QModelIndex filterIdx = m_connGroupProxy
                                          ? m_connGroupProxy->mapToSource(viewIdx)
                                          : viewIdx;
        const QModelIndex srcIdx = filterIdx.isValid()
                                       ? m_connProxy->mapToSource(filterIdx)
                                       : QModelIndex{};
        if (srcIdx.isValid()) {
            const int row = srcIdx.row();

            const QString srcText  = m_connModel->copyTextForEndpoint(
                row, ConnectionModel::FlowEnd::Source);
            const QString dstText  = m_connModel->copyTextForEndpoint(
                row, ConnectionModel::FlowEnd::Destination);
            const QString lineText = m_connModel->copyTextForFlow(row);
            const QString peer     = m_connModel->peerAddressText(row);

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

            if (!peer.isEmpty()) {
                menu.addSeparator();
                // The captured `pos` is in viewport coordinates relative to
                // m_connView and is the same point we'll re-resolve to a row
                // when the user clicks the action; this stays correct even
                // if the proxy resorts before the action fires.
                const QPoint capturePos = pos;
                auto *only    = menu.addAction(tr("Show only flows to/from %1").arg(peer));
                auto *exclude = menu.addAction(tr("Hide flows to/from %1").arg(peer));
                connect(only,    &QAction::triggered, this,
                        [this, capturePos] { filterByConnectionRow(capturePos, /*exclude=*/false); });
                connect(exclude, &QAction::triggered, this,
                        [this, capturePos] { filterByConnectionRow(capturePos, /*exclude=*/true);  });
            }

            // ---- Attribution section ------------------------------------
            // Pull the structured attribution fields directly off the
            // model's per-row roles so we don't depend on the (possibly
            // hidden) Process / Container columns being visible.
            const QModelIndex anchor = m_connModel->index(row, 0);
            const qint32 pid = m_connModel
                ->data(anchor, ConnectionModel::ProcessPidRole).toInt();
            const QString comm = m_connModel
                ->data(anchor, ConnectionModel::ProcessCommRole).toString();
            const QString cRuntime = m_connModel
                ->data(anchor, ConnectionModel::ContainerRuntimeRole).toString();
            const QString cId   = m_connModel
                ->data(anchor, ConnectionModel::ContainerIdRole).toString();
            const QString cName = m_connModel
                ->data(anchor, ConnectionModel::ContainerNameRole).toString();

            const bool hasProcess   = pid > 0 || !comm.isEmpty();
            const bool hasContainer = !cRuntime.isEmpty()
                                      || !cId.isEmpty() || !cName.isEmpty();

            // Quote a value for the filter mini-language. The parser
            // treats double-quoted strings as one token, so this is safe
            // for names with spaces / colons / parens. Backslash-escape
            // any embedded quotes.
            const auto quoteForFilter = [](const QString &raw) {
                QString s = raw;
                s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
                s.replace(QLatin1Char('"'),  QStringLiteral("\\\""));
                return QStringLiteral("\"%1\"").arg(s);
            };
            const auto setFilter = [this](const QString &clause) {
                if (!m_connFilterEdit) return;
                m_connFilterEdit->setText(clause);
                m_connFilterEdit->setFocus(Qt::OtherFocusReason);
            };

            if (hasProcess || hasContainer) {
                menu.addSeparator();
            }

            if (hasProcess) {
                const QString commLabel = comm.isEmpty()
                    ? tr("[pid %1]").arg(pid) : comm;
                // Prefer comm match (`comm:<x>`) when available — pids
                // are recycled and short-lived. Fall back to exact pid
                // when we don't have a comm (rare but possible).
                auto *byProc = menu.addAction(
                    tr("Filter by process: %1").arg(commLabel));
                if (!comm.isEmpty()) {
                    const QString q = quoteForFilter(comm);
                    connect(byProc, &QAction::triggered, this,
                            [setFilter, q] {
                                setFilter(QStringLiteral("comm=%1").arg(q));
                            });
                } else {
                    connect(byProc, &QAction::triggered, this,
                            [setFilter, pid] {
                                setFilter(QStringLiteral("pid=%1").arg(pid));
                            });
                }

                auto *copyProc = menu.addAction(
                    tr("Copy process info"));
                connect(copyProc, &QAction::triggered, this,
                        [copyToClip, comm, pid] {
                            copyToClip(QStringLiteral("%1 [pid %2]")
                                .arg(comm.isEmpty() ? QStringLiteral("?") : comm)
                                .arg(pid));
                        });
            }

            if (hasContainer) {
                const QString containerLabel = !cName.isEmpty() ? cName
                                              : !cId.isEmpty()   ? cId
                                              : cRuntime;
                // Container filter: prefer name (stable, human-meaningful);
                // fall back to id; final fallback is runtime-only which
                // matches every flow in that runtime.
                auto *byCont = menu.addAction(
                    tr("Filter by container: %1").arg(containerLabel));
                QString matchToken = !cName.isEmpty() ? cName
                                    : !cId.isEmpty()   ? cId
                                    : cRuntime;
                const QString q = quoteForFilter(matchToken);
                connect(byCont, &QAction::triggered, this,
                        [setFilter, q] {
                            setFilter(QStringLiteral("container:%1").arg(q));
                        });

                if (!cRuntime.isEmpty()) {
                    auto *byRt = menu.addAction(
                        tr("Filter by runtime: %1").arg(cRuntime));
                    const QString rq = quoteForFilter(cRuntime);
                    connect(byRt, &QAction::triggered, this,
                            [setFilter, rq] {
                                setFilter(QStringLiteral("runtime=%1").arg(rq));
                            });
                }

                auto *copyCont = menu.addAction(
                    tr("Copy container info"));
                connect(copyCont, &QAction::triggered, this,
                        [copyToClip, cRuntime, cId, cName] {
                            QStringList parts;
                            if (!cRuntime.isEmpty()) parts << cRuntime;
                            if (!cName.isEmpty())    parts << cName;
                            if (!cId.isEmpty())      parts << cId;
                            copyToClip(parts.join(QLatin1Char(' ')));
                        });
            }
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

namespace {

// Build a header context menu populated with one checkable action per
// logical column, plus a "Reset to defaults" item that re-shows every
// column. Used by both the interfaces- and connections-table headers
// (the per-view "default visible" calculation may differ — pass in
// alwaysHidden for columns whose visibility is governed by other
// settings, e.g. the RxMax/TxMax gauge columns).
void populateHeaderMenu(QMenu *menu, QHeaderView *header,
                        const QSet<int> &alwaysHidden = {})
{
    QAbstractItemModel *m = header->model();
    if (!m) return;
    const int cols = m->columnCount();

    // Count currently-visible sections so we can disable the toggle of
    // the last one — hiding every column would orphan the view with no
    // way to summon the menu back.
    int visibleCount = 0;
    for (int i = 0; i < cols; ++i)
        if (!header->isSectionHidden(i)) ++visibleCount;

    for (int i = 0; i < cols; ++i) {
        const QString label = m->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction *act = menu->addAction(label.isEmpty()
            ? QStringLiteral("Column %1").arg(i + 1) : label);
        act->setCheckable(true);
        act->setChecked(!header->isSectionHidden(i));
        if (act->isChecked() && visibleCount <= 1)
            act->setEnabled(false);  // refuse to hide the last visible column
        QObject::connect(act, &QAction::toggled, header,
            [header, i](bool checked) { header->setSectionHidden(i, !checked); });
    }

    menu->addSeparator();
    QAction *resetAct = menu->addAction(QObject::tr("Reset columns to defaults"));
    QObject::connect(resetAct, &QAction::triggered, header,
        [header, cols, alwaysHidden] {
            for (int i = 0; i < cols; ++i)
                header->setSectionHidden(i, alwaysHidden.contains(i));
            // Also restore the original logical order. Walk visual->
            // logical and move each into its logical slot.
            for (int logical = 0; logical < cols; ++logical) {
                const int visual = header->visualIndex(logical);
                if (visual != logical)
                    header->moveSection(visual, logical);
            }
        });
}

} // namespace

void MainWindow::showNetHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    populateHeaderMenu(&menu, m_netView->horizontalHeader());
    menu.exec(m_netView->horizontalHeader()->viewport()->mapToGlobal(pos));
}

void MainWindow::showConnHeaderMenu(const QPoint &pos)
{
    // RxMax/TxMax visibility is governed by the throughput-gauge toggle
    // in applySettingsToUi(); pin them as "default hidden" so Reset does
    // the same thing.
    const QSet<int> alwaysHidden{
        static_cast<int>(ConnectionModel::Column::RxMax),
        static_cast<int>(ConnectionModel::Column::TxMax),
    };
    QMenu menu(this);
    populateHeaderMenu(&menu, m_connView->header(), alwaysHidden);
    menu.exec(m_connView->header()->viewport()->mapToGlobal(pos));
}
