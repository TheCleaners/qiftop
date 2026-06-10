#pragma once

#include <QObject>

#include "tui/Screen.h"
#include "tui/TuiFormat.h"

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
           QObject *parent = nullptr);

    // Called by main's QSocketNotifier after draining curses input.
    void handleKey(int key);

    // Schedule a throttled repaint (coalesces bursts of change signals).
    void requestRedraw();

private:
    void  doRedraw();
    Frame buildFrame();
    void  buildModal(Frame &f) const;          // fill f.modal for the open overlay
    void  handleSettingsKey(int key);          // key routing while Settings open
    void  handleFieldsKey(int key);            // key routing while Fields open
    void  applyAggregatorSettings();           // push flags into the aggregator

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

    // Modal overlays (only one open at a time). Settings = runtime toggles,
    // Fields = top-style sort-column selector.
    enum class Overlay { None, Settings, Fields };
    Overlay m_overlay = Overlay::None;
    int  m_settingsSel    = 0;
    int  m_fieldsSel      = 0;

    // Runtime-toggleable settings (the modal). Defaults mirror main.cpp's
    // initial aggregator wiring (smoothing on @ 300ms, UDP-by-peer on).
    bool m_gaugeEnabled   = true;
    bool m_dnsEnabled     = false;
    bool m_udpAggregate   = true;
    bool m_smoothing      = true;
    int  m_pollMs         = 1000;

    QTimer *m_redrawTimer = nullptr; // single-shot throttle
    QTimer *m_smoothTimer = nullptr; // advanceSmoothing tick
    bool    m_redrawPending = false;
};

} // namespace qiftop::tui
