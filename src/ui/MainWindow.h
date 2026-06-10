#pragma once

#include <QMainWindow>

#include "backend/ConnectionMonitor.h"
#include "backend/NetworkMonitor.h"

class ConnectionModel;
class ConnectionFilterProxy;
class ConnectionFlowDelegate;
class DnsResolver;
class InterfaceFilterProxy;
class NetworkModel;
class Settings;
class TrayManager;
class QFrame;
class QTimer;
class QLabel;
class QTableView;
class QTreeView;
class QAbstractItemView;
class QTabWidget;
class QToolBar;
class QMenu;

namespace util {
class HandoffClient;
class HandoffServer;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(Settings           *settings,
               NetworkMonitor     *netMonitor,
               ConnectionMonitor  *connMonitor,
               DnsResolver        *dnsResolver,
               QWidget            *parent = nullptr);
    ~MainWindow() override;

    // Called by main.cpp on the privileged child: every emitted statsUpdated
    // is mirrored to the parent over the IPC channel, and tray-style commands
    // (Show / Pause / Quit) coming from the parent are applied locally.
    void attachHandoffClient(util::HandoffClient *client);

    // Called on the unprivileged parent right before relaunching as admin.
    // Once the child sends READY we drop our own data collection and start
    // forwarding the tray's user actions through the IPC channel instead.
    void prepareProxyMode(util::HandoffServer *server);

    // Called by main.cpp once the data source is chosen so the status bar
    // can show whether we're consuming the privileged DBus agent (and which
    // version of its contract) or running the in-process fallback. `version`
    // and `caps` are empty for the in-process path and for pre-property
    // agents; the label degrades gracefully.
    void setBackendInfo(bool usingAgent,
                        const QString     &version,
                        const QStringList &caps);

    // Returns the agent metadata captured by setBackendInfo(). Used by the
    // Help → About dialog so it can show "Connected to qiftop-agent vX.Y"
    // alongside the application version. Empty fields when running on the
    // in-process backend (or against a pre-property agent).
    [[nodiscard]] bool        usingAgent()        const { return m_usingAgent; }
    [[nodiscard]] QString     agentVersion()      const { return m_agentVersion; }
    [[nodiscard]] QStringList agentCapabilities() const { return m_agentCaps; }

    // Called by main.cpp when the data source is the DBus agent and has
    // notified us of a change in its effective polling cadence (sped up,
    // slowed down, or paused with ms==0). We tint the backend status-bar
    // label so the user can see at a glance that the agent has gone idle
    // without scrolling the journal.
    void notifyAgentCadence(int intervalMs);

private slots:
    void onStatsUpdated(const QList<InterfaceStats> &stats);
    void onConnectionsUpdated(const QList<Connection> &conns);
    void onSettingsChanged();
    void togglePaused(bool paused);
    void openSettingsDialog();
    void showInterfaceContextMenu(const QPoint &pos);
    void showConnectionContextMenu(const QPoint &pos);
    void showNetHeaderMenu(const QPoint &pos);
    void showConnHeaderMenu(const QPoint &pos);
    void quitFromTray();
    void onConnectionsPermissionDenied(const QString &detail);
    void onConnectionsAccountingUnavailable(const QString &detail);
    void relaunchAsAdmin();
    void onConnFilterTextChanged(const QString &text);
    void applyConnFilterExpr();
    void showFilterHelp();
    void showAboutDialog();
    void showShortcutsDialog();

private:
    enum class ExportFormat { Json, Csv };
    enum class ExportSink   { File, Clipboard };

    void setupUi();
    void setupMenuAndToolbar();
    void updateStatusBar(const QList<InterfaceStats> &stats);
    void applySettingsToUi();
    void runExport(class Exportable *src, ExportFormat fmt,
                   ExportSink sink, const QString &baseName);

    void rebuildConnIfaceFilterMenu(const QList<InterfaceStats> &stats);
    void applyConnIfaceFilterToProxy();
    void updateConnIfaceFilterVisibility();

    // Appends a "View" section to a context menu: toggles for menu bar /
    // toolbar visibility and a Preferences shortcut. Shared between the
    // Interfaces and Connections tab context menus.
    void appendViewToggleSection(QMenu *menu);

    // Recomputes the window title from current settings. When
    // Settings::showStatusInTitle() is true, suffixes the base title
    // with a compact status string (poll interval, iface filter,
    // protocol/family/direction state). No-op otherwise (resets to base).
    void updateWindowTitle();

public:
    // Switch focus to the Connections tab. Used by the -i command-line
    // option and could be useful from a future tray/IPC action.
    void selectConnectionsTab();

private:
    void readUiState();
    void writeUiState();
    void installShortcuts();
    // Copies the currently-selected rows of `view` to the clipboard,
    // formatted via the matching Exportable model's CSV emitter for
    // selected rows, or via copyTextForFlow() for connections.
    void copyTableSelectionToClipboard(QAbstractItemView *view);
    // Sets the Connections filter expression to one isolating (or
    // excluding) the peer at the row under `pos` in m_connView.
    void filterByConnectionRow(const QPoint &pos, bool exclude);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    Settings          *m_settings    = nullptr;
    NetworkMonitor    *m_netMonitor  = nullptr;
    ConnectionMonitor *m_connMonitor = nullptr;
    QTimer            *m_agentHeartbeat = nullptr;
    QTimer            *m_smoothingTick  = nullptr;
    DnsResolver       *m_dnsResolver = nullptr;
    TrayManager       *m_tray        = nullptr;

    bool m_explicitQuit = false; // set by tray's Quit action so closeEvent exits
    bool m_proxyMode    = false; // parent is proxying for a privileged child

    // Backend metadata captured by setBackendInfo() and exposed via the
    // Help → About dialog.
    bool        m_usingAgent = false;
    QString     m_agentVersion;
    QStringList m_agentCaps;

    // Interfaces tab
    NetworkModel         *m_netModel       = nullptr;
    InterfaceFilterProxy *m_netProxy       = nullptr;
    QTableView           *m_netView        = nullptr;

    // Connections tab
    ConnectionModel       *m_connModel     = nullptr;
    ConnectionFilterProxy *m_connProxy     = nullptr;
    class ConnectionGroupProxy *m_connGroupProxy = nullptr;
    // On-demand process details (exe/cmdline/cwd) keyed by pid, populated
    // lazily from the agent's GetProcessDetails RPC; the group proxy
    // reads it for ByProcess group tooltips.
    QHash<qint32, qiftop::backend::ProcessDetails> m_procDetails;
    QTreeView             *m_connView      = nullptr;
    ConnectionFlowDelegate *m_connFlowDelegate = nullptr;
    QFrame                *m_connBanner    = nullptr; // shown on EPERM
    QLabel                *m_connBannerLbl = nullptr;
    // Shared "filter connections by interface" menu. Owned by the window;
    // installed both as a popup on a toolbar QToolButton (visible only on
    // the Connections tab) and as a submenu under View → "Show
    // connections on" (disabled when the Connections tab is not active).
    class QMenu           *m_connIfaceFilterMenu       = nullptr;
    class QToolButton     *m_connIfaceFilterBtn        = nullptr; // toolbar trigger
    QAction               *m_connIfaceFilterToolbarAct = nullptr; // QAction wrapping the button
    QAction               *m_connIfaceFilterMenuAct    = nullptr; // QAction wrapping the submenu
    class QComboBox       *m_connViewModeCombo         = nullptr;
    QAction               *m_connViewModeToolbarAct    = nullptr;
    class QLineEdit       *m_connFilterEdit            = nullptr;
    QAction               *m_connFilterToolbarAct      = nullptr;
    QAction               *m_connFilterSepAct          = nullptr;
    QAction               *m_connFilterSpacerAct       = nullptr;
    class QFrame          *m_filterHelpPopup           = nullptr;
    QTimer                *m_connFilterDebounce        = nullptr;
    QWidget               *m_connTab                   = nullptr; // for tab-change detection

    QTabWidget *m_tabs = nullptr;

    QAction *m_pauseAction    = nullptr;
    QAction *m_settingsAction = nullptr;
    QAction *m_quitAction     = nullptr;
    QAction *m_closeAction    = nullptr;   // Hide-to-tray; visible only when closeToTray is enabled.
    QAction *m_menuBarToggleAction = nullptr;
    QToolBar *m_toolbar       = nullptr;

    // Export actions, shared between the toolbar dropdown and File→Export.
    QAction *m_exportIfacesJsonAct = nullptr;
    QAction *m_exportIfacesCsvAct  = nullptr;
    QAction *m_exportConnsJsonAct  = nullptr;
    QAction *m_exportConnsCsvAct   = nullptr;
    QAction *m_copyIfacesJsonAct   = nullptr;
    QAction *m_copyIfacesCsvAct    = nullptr;
    QAction *m_copyConnsJsonAct    = nullptr;
    QAction *m_copyConnsCsvAct     = nullptr;

    QLabel *m_statusInterfaces = nullptr;
    QLabel *m_statusConnections = nullptr;
    QLabel *m_statusThroughput  = nullptr;
    QLabel *m_statusBackend     = nullptr;

    // Centered "no flows" placeholder shown over the Connections view
    // when the model is empty. Parented to the view's viewport so it
    // tracks size + alternating-row background naturally.
    QLabel *m_connEmptyOverlay  = nullptr;

    bool m_paused = false;
};
