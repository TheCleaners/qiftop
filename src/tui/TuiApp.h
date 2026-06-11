#pragma once

#include <QObject>
#include <QSet>

#include <functional>

#include "tui/Screen.h"
#include "tui/TuiFormat.h"
#include "util/ConnectionFilter.h"
#include "aggregate/InterfaceAggregator.h"
#include "aggregate/ConnectionAggregator.h"

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
    void  handleDetailKey(int key);            // key routing while a row Detail is open
    void  openDetail();                        // open the Detail overlay for the cursor row
    void  exportCurrentView();                 // write the active view to a CSV file
    void  flashMessage(const QString &msg);    // transient footer status (cleared on a timer)
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

    // Per displayed row of the active view, rebuilt each buildFrame: whether the
    // row is a cursor-landable target, whether it's a group header, its stable
    // identity key (flows) and the group key it belongs to. Drives cursor
    // navigation, the Detail overlay, and group collapse/expand.
    struct RowRef {
        bool    selectable = false; // cursor may land here (headers AND members)
        bool    header     = false; // true for a group header row
        QString key;                // flow identity (members only)
        QString groupKey;           // owning group key (headers and members)
    };
    QList<RowRef> m_rowRefs;
    void moveCursor(int delta);   // move selection over landable rows

    // Group keys the user has collapsed (Connections view, when grouped). Keyed
    // by the same group key buildFrame buckets on, so collapse survives the
    // per-tick frame rebuild. Cleared when the grouping mode changes.
    QSet<QString> m_collapsedGroups;
    // When set, after the next buildFrame the cursor is moved onto this group's
    // header row (used so collapsing keeps the cursor on the folded header).
    // A separate validity flag is needed because the empty string is itself a
    // valid group key (the "(unattributed)" / "(no container)" bucket).
    QString m_cursorTargetGroup;
    bool    m_cursorTargetValid = false;
    void collapseAtCursor();      // h/Left: fold the cursor's group
    void expandAtCursor();        // l/Right on a collapsed header: unfold
    void toggleCollapseAtCursor();// Enter/Space on a header: fold/unfold
    [[nodiscard]] bool cursorOnHeader() const;

    // Detail overlay target: the stable key + view of the row whose details are
    // shown. Rebuilt live each frame from the current rows so rates stay fresh.
    QString m_detailKey;
    View    m_detailView = View::Connections;

    // Modal overlays (only one open at a time). Settings = runtime toggles,
    // Fields = top-style sort-column selector, Help/About = read-only info,
    // Detail = the per-row inspector (replaces inline expansion).
    enum class Overlay { None, Settings, Fields, Help, About, Detail };
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
    // Snapshot captured at pause time. While paused we render from this frozen
    // copy, not the live aggregator, so the view does not shift/resort as data
    // keeps arriving in the background (a key-driven redraw would otherwise
    // re-read the mutated live rows and move the cursor row out from under us).
    QList<aggregate::InterfaceAggregator::Row>  m_frozenIfaceRows;
    QList<aggregate::ConnectionAggregator::Row> m_frozenConnRows;

    // Transient one-line status (e.g. "Exported 42 flows to ..."), shown in
    // the footer and cleared after a few seconds by m_flashTimer.
    QString  m_flashMsg;
    QTimer  *m_flashTimer = nullptr;

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
