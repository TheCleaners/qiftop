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
    QList<Role>   rowRoles;          // per-row colour role (parallel to rows)
    QList<double> rowGauge;          // per-row bandwidth fraction [0,1] (parallel)
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

    // Install a theme. May be called before or after init(); colour pairs are
    // (re)registered if the terminal supports colour.
    void setTheme(const Theme &theme);
    [[nodiscard]] const QString &themeName() const { return m_theme.name; }

    [[nodiscard]] int  rows() const;   // terminal height
    [[nodiscard]] int  cols() const;   // terminal width
    // Body height available for table rows (total minus chrome: tab line,
    // column header, footer).
    [[nodiscard]] int  bodyHeight() const;

    [[nodiscard]] int  pollKey();      // wgetch (nodelay); ERR (-1) if none

    void render(const Frame &f);

private:
    void applyTheme();                 // (re)register colour pairs
    long attrFor(Role r) const;        // COLOR_PAIR | A_* for a role
    // Paint the row-spanning bandwidth gauge: fill the first `fraction` of
    // line `y` with the gauge tint (256-colour) or reverse-video (fallback).
    void paintGauge(int y, int width, double fraction, Role role) const;

    bool  m_active   = false;
    bool  m_hasColor = false;
    bool  m_color256 = false;
    Theme m_theme    = builtinThemes().first();
};

} // namespace qiftop::tui
