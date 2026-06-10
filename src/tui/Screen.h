#pragma once

// ncursesw rendering seam for nqiftop. All raw curses calls live here so the
// rest of the app is curses-agnostic (a notcurses backend could replace this
// class). It owns the terminal lifecycle (init/shutdown) and paints a Frame.

#include <QList>
#include <QString>
#include <QStringList>

#include "tui/TuiFormat.h"

namespace qiftop::tui {

// Everything Screen needs to paint one frame. TuiApp builds this each redraw.
struct Frame {
    QStringList   tabs;              // e.g. {"Interfaces", "Connections"}
    int           activeTab = 0;
    QString       sourceLabel;       // "agent 0.2.1" / "in-process" / "no source"
    QList<Column> columns;           // header + alignment for the active view
    int           sortCol  = 0;
    bool          sortDesc = true;
    QList<QStringList> rows;         // ALL rows (already sorted), each = cells
    int           scrollOffset = 0;  // index of the first visible body row
    QString       footer;            // key help line
};

class Screen {
public:
    Screen() = default;
    ~Screen();

    void init();                       // setlocale + initscr + raw/noecho/...
    void shutdown();                   // endwin (idempotent, signal-safe)
    [[nodiscard]] bool active() const { return m_active; }

    [[nodiscard]] int  rows() const;   // terminal height
    [[nodiscard]] int  cols() const;   // terminal width
    // Body height available for table rows (total minus chrome: tab line,
    // column header, footer).
    [[nodiscard]] int  bodyHeight() const;

    [[nodiscard]] int  pollKey();      // wgetch (nodelay); ERR (-1) if none

    void render(const Frame &f);

private:
    bool m_active = false;
};

} // namespace qiftop::tui
