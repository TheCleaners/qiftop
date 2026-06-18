#pragma once

// GUI colour themes for the qiftop Qt Widgets client.
//
// The analogue of the nqiftop TUI theme model (src/tui/TuiTheme.h), but
// expressed in Qt's own currency — a full QPalette — instead of ncurses
// colour numbers. A theme is a named bundle of (palette + a pair of flow
// accent colours for the Connections "Flow" column). The first/default
// theme is "System": followSystem=true means "don't touch anything", so
// out-of-the-box appearance is identical to a build with no theming at all.
//
// Deliberately QtGui-only (QPalette/QColor/QString — no QWidget), so the
// model is unit-testable headlessly (QPalette works under QGuiApplication
// with the offscreen platform). The applier (applyGuiTheme / the system
// capture-restore) is declared here but its definition pulls in
// QApplication/QStyleFactory; keep those includes in the .cpp.

#include <QColor>
#include <QList>
#include <QPalette>
#include <QString>

namespace qiftop::ui {

struct GuiTheme {
    QString  name;                  // "System", "Dark", "Light", "Nord", …
    bool     followSystem = false;  // true ONLY for "System": never overrides
    QPalette palette;               // full palette (ignored when followSystem)
    QColor   flowSource;            // accent for the outbound/source endpoint
    QColor   flowDest;              // accent for the inbound/dest endpoint
};

// The built-in set. "System" is always first (and is the default). The rest
// are real palettes: Dark, Light, Nord, Solarized Dark, Solarized Light,
// Gruvbox Dark.
[[nodiscard]] QList<GuiTheme> builtinGuiThemes();

// Case-insensitive lookup by name. Returns -1 when no theme matches (e.g. a
// persisted name from a future build) so callers can fall back to "System".
[[nodiscard]] int guiThemeIndexByName(const QList<GuiTheme> &themes,
                                      const QString &name);

// --- Application -----------------------------------------------------------
//
// Capture the process's ORIGINAL style name + palette exactly once, as early
// as possible in main() (before any theme is applied), so the "System" theme
// can restore the user's native look. Idempotent: only the first call records
// anything. Safe to call with no QApplication yet (records empty/defaults).
void captureSystemTheme();

// Apply a theme live to the running QApplication. For followSystem themes we
// restore the captured native style + palette. For a real theme we force the
// Fusion style (the only widely-available style that fully honours an
// arbitrary QPalette — native styles such as Windows/macOS/Breeze ignore most
// palette colours) and set the theme's palette. Existing top-level widgets
// re-polish automatically via the ApplicationPaletteChange/StyleChange events
// Qt posts, so no restart is needed.
void applyGuiTheme(const GuiTheme &theme);

} // namespace qiftop::ui
