#pragma once

#include <QObject>
#include <QSet>

#include <functional>

#include "tui/Screen.h"
#include "tui/TuiFormat.h"
#include "util/ConnectionFilter.h"

class QTimer;

namespace qiftop::aggregate {
class InterfaceAggregator;
class ConnectionAggregator;
} // namespace qiftop::aggregate

namespace qiftop::tui {

// Controller for the ncurses frontend: owns the view/sort/scroll state, drives
// a throttled redraw + the rate-smoothing tick, and translates key input into
// state changes. It reads the libqiftop aggregators and asks Screen to paint.
class TuiApp : public QObject {
    Q_OBJECT

public:
    TuiApp(Screen *screen,
           aggregate::InterfaceAggregator  *ifaceAgg,
           aggregate::ConnectionAggregator *connAgg,
           QString sourceLabel,
           QString themeName,
           int pollMs,
           QString viewName = {},   // CLI override: "interfaces"/"connections"
           QString groupName = {},  // CLI override: off/interface/process/container
           QObject *parent = nullptr);

    // Called by main's QSocketNotifier after draining curses input.
    void handleKey(int key);

    // Schedule a throttled repaint (coalesces bursts of change signals).
    void requestRedraw();

    // Install a callback that applies a new poll interval (ms) to the data
    // source (the monitors live in main). Applies the current interval once.
    void setPollApplier(std::function<void(int)> fn);
    [[nodiscard]] int pollIntervalMs() const { return m_pollMs; }

private:
    // A single declarative, runtime-adjustable preference. value() renders the
    // current value; adjust(dir) changes it (dir = -1 left, +1 right/activate).
    // This keeps the settings modal extensible — add a row, not a switch case.
    struct SettingItem {
        QString                  label;
        QString                  help;
        std::function<QString()> value;
        std::function<void(int)> adjust;
    };

    void  doRedraw();
    Frame buildFrame();
    void  buildModal(Frame &f) const;          // fill f.modal for the open overlay
    void  buildSettingItems();                 // (re)build the declarative list
    void  applyPollInterval();                 // push m_pollMs to agg/timer/source
    void  handleSettingsKey(int key);          // key routing while Settings open
    void  handleFieldsKey(int key);            // key routing while Fields open
    void  handleInfoKey(int key);              // key routing while Help/About open
    void  applyAggregatorSettings();           // push flags into the aggregator
    void  loadSettings();                      // restore view/sort/toggles/theme
    void  saveSettings() const;                // persist them (QSettings)
    void  onDataChanged();                     // data-driven redraw (paused-aware)
    void  handleFilterKey(int key);            // key routing while editing a filter
    void  commitFilter();                      // parse m_filterDraft -> m_filterExpr

    Screen                           *m_screen   = nullptr;
    aggregate::InterfaceAggregator   *m_ifaceAgg = nullptr;
    aggregate::ConnectionAggregator  *m_connAgg  = nullptr;
    QString                           m_sourceLabel;

    QList<Theme> m_themes;
    int          m_themeIdx = 0;

    View m_view = View::Connections;
    int  m_ifaceSortCol = 0;  bool m_ifaceSortDesc = false; // name asc
    int  m_connSortCol  = 1;  bool m_connSortDesc  = true;  // RX rate desc
    int  m_ifaceScroll  = 0;
    int  m_connScroll   = 0;
    int  m_ifaceCursor  = 0;  // selected row (index into displayed rows)
    int  m_connCursor   = 0;

    // Expanded rows (aptitude-style detail tree). Keyed by stable identity
    // (interface name / connection 5-tuple) so they survive a re-sort/refresh.
    QSet<QString> m_expandedIface;
    QSet<QString> m_expandedConn;
    // Per displayed row of the active view, rebuilt each buildFrame: whether the
    // row can be expanded and its identity key. Lets handleKey act on the cursor.
    struct RowRef { bool expandable = false; QString key; };
    QList<RowRef> m_rowRefs;
    void moveCursor(int delta);   // move selection, clamped, scroll follows
    void toggleExpand(int dir);   // dir: 0 toggle, +1 expand, -1 collapse

    // Modal overlays (only one open at a time). Settings = runtime toggles,
    // Fields = top-style sort-column selector, Help/About = read-only info.
    enum class Overlay { None, Settings, Fields, Help, About };
    Overlay m_overlay = Overlay::None;
    int  m_settingsSel    = 0;
    int  m_fieldsSel      = 0;

    // Runtime-toggleable settings (the modal). Defaults mirror main.cpp's
    // initial aggregator wiring (smoothing on @ 300ms, UDP-by-peer on).
    bool m_gaugeEnabled   = true;
    bool m_dnsEnabled     = false;
    bool m_udpAggregate   = true;
    bool m_smoothing      = true;
    GroupBy m_groupBy     = GroupBy::None;
    bool m_directionColors = true;   // colour rows by flow direction
    int  m_pollMs         = 1000;
    std::function<void(int)> m_applyPollMs;  // set by main (owns the monitors)
    QList<SettingItem> m_settings;           // declarative settings model

    // Pause: freeze live updates so the snapshot can be read.
    bool m_paused = false;

    // Filter (Connections view): a live ConnectionFilter mini-language query.
    bool                  m_filterEditing = false;
    QString               m_filterDraft;   // text being typed
    QString               m_filterText;    // committed text
    qiftop::filter::ExprPtr m_filterExpr;  // parsed (null = match-all)
    QString               m_filterError;   // last parse error (empty = ok)

    QTimer *m_redrawTimer = nullptr; // single-shot throttle
    QTimer *m_smoothTimer = nullptr; // advanceSmoothing tick
    bool    m_redrawPending = false;
};

} // namespace qiftop::tui
