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

    QTimer *m_redrawTimer = nullptr; // single-shot throttle
    QTimer *m_smoothTimer = nullptr; // advanceSmoothing tick
    bool    m_redrawPending = false;
};

} // namespace qiftop::tui
