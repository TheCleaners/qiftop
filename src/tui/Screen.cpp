#include "tui/Screen.h"

#include <clocale>
#include <cmath>
#include <cwchar>
#include <langinfo.h>
#include <string>

// ncurses last: it defines lower-case macros (erase, move, refresh, timeout…)
// that would clash with Qt/STL identifiers if included before them.
#include "tui/Curses.h"

namespace qiftop::tui {

namespace {

constexpr int kSep       = 2;   // gap between columns
constexpr int kNumColCap = 13;  // max width for a numeric column

// Truncate/pad `text` to exactly `width` display cells (approx: 1 cell per
// QChar, which holds for our mostly-ASCII content + the single → glyph).
QString fitCell(const QString &text, int width, bool rightAlign)
{
    if (width <= 0)
        return {};
    QString t = text;
    if (t.size() > width) {
        if (width >= 1)
            t = t.left(width - 1) + QChar(0x2026); // …
        else
            t = t.left(width);
    }
    const int pad = width - t.size();
    if (pad <= 0)
        return t;
    const QString spaces(pad, QLatin1Char(' '));
    return rightAlign ? spaces + t : t + spaces;
}

// Map the backend-agnostic attr bits to ncurses A_* attributes.
long ncursesAttr(int bits)
{
    long a = A_NORMAL;
    if (bits & attr::Bold)      a |= A_BOLD;
    if (bits & attr::Dim)       a |= A_DIM;
    if (bits & attr::Reverse)   a |= A_REVERSE;
    if (bits & attr::Underline) a |= A_UNDERLINE;
    return a;
}

void putLine(int y, const QString &s);

// Convert a QString to a wchar_t string. On Linux/BSD wchar_t is 4 bytes, so
// QString::toWCharArray emits one UCS-4 code point per character — exactly
// what the wide curses API wants.
std::wstring toWide(const QString &s)
{
    std::wstring w(s.size() + 1, L'\0');
    const int n = s.toWCharArray(w.data());
    w.resize(n < 0 ? 0 : n);
    return w;
}

// Write `s` at (y, x) through the WIDE curses API. The narrow addstr() path
// pushes raw UTF-8 bytes, which only renders correctly where the curses lib
// does multibyte→cell decoding (Linux ncurses). BSD base curses places each
// byte in its own cell, garbling box-drawing and other multibyte glyphs.
// add_wch/addwstr is portable across ncurses and BSD curses.
void putAt(int y, int x, const QString &s)
{
    const std::wstring w = toWide(s);
    mvaddwstr(y, x, w.c_str());
}

void putLine(int y, const QString &s)
{
    putAt(y, 0, s);
}

} // namespace

Screen::~Screen() { shutdown(); }

void Screen::init()
{
    if (m_active)
        return;
    // Wide-character curses output requires a UTF-8 LC_CTYPE. Honour the
    // user's environment first; if that yields a non-UTF-8 codeset (e.g. a
    // bare "C"/POSIX locale over SSH), fall back to a known UTF-8 locale so
    // box-drawing and other multibyte glyphs still render. Without this the
    // wide API would fail to encode non-ASCII and the frames garble.
    std::setlocale(LC_ALL, "");
    if (const char *cs = nl_langinfo(CODESET); !cs || std::string(cs) != "UTF-8") {
        for (const char *loc : {"C.UTF-8", "en_US.UTF-8", "POSIX.UTF-8"}) {
            if (std::setlocale(LC_ALL, loc)) {
                std::setlocale(LC_CTYPE, loc);
                break;
            }
        }
    }
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        m_hasColor = true;
        m_color256 = (COLORS >= 256);
    }
    m_active = true;
    applyTheme();
}

void Screen::shutdown()
{
    if (!m_active)
        return;
    endwin();
    m_active = false;
}

void Screen::setTheme(const Theme &theme)
{
    m_theme = theme;
    if (m_active)
        applyTheme();
}

void Screen::applyTheme()
{
    if (!m_active || !m_hasColor)
        return;
    const int n = static_cast<int>(Role::Count);
    for (int i = 0; i < n; ++i) {
        const ThemeColor &c = m_theme.roles[i];
        init_pair(static_cast<short>(i + 1),
                  static_cast<short>(c.fg),
                  static_cast<short>(c.bg));
    }
    // Gauge-fill pairs (256-colour only): same role fg, the theme's gauge
    // background tint — a subtle fill behind readable text. Pairs n+1..2n.
    if (m_color256 && m_theme.gaugeBg >= 0) {
        for (int i = 0; i < n; ++i)
            init_pair(static_cast<short>(n + 1 + i),
                      static_cast<short>(m_theme.roles[i].fg),
                      static_cast<short>(m_theme.gaugeBg));
    }
}

int Screen::paintGauge(int y, int width, double fraction, Role role, long extra) const
{
    if (!m_active || width <= 0 || fraction <= 0.0)
        return 0;
    int fill = static_cast<int>(std::lround(fraction * width));
    if (fill <= 0)
        fill = 1;            // a visible sliver for any non-zero traffic
    if (fill > width)
        fill = width;
    const int r = static_cast<int>(role);
    if (m_color256 && m_theme.gaugeBg >= 0) {
        // Re-colour the filled cells to (role fg, gauge bg) without touching
        // the glyphs — the Qt RowGaugeDelegate "tint behind text" look.
        mvchgat(y, 0, fill, extra | ncursesAttr(m_theme[role].attr),
                static_cast<short>(static_cast<int>(Role::Count) + 1 + r), nullptr);
    } else {
        // Fallback for 8-colour / mono: reverse-video the filled region.
        const short pair = m_hasColor ? static_cast<short>(r + 1) : 0;
        mvchgat(y, 0, fill, extra | A_REVERSE | ncursesAttr(m_theme[role].attr), pair, nullptr);
    }
    return fill;
}

long Screen::attrFor(Role r) const
{
    const ThemeColor &c = m_theme[r];
    long a = ncursesAttr(c.attr);
    if (m_hasColor)
        a |= COLOR_PAIR(static_cast<int>(r) + 1);
    return a;
}

int Screen::rows() const { return m_active ? getmaxy(stdscr) : 0; }
int Screen::cols() const { return m_active ? getmaxx(stdscr) : 0; }

int Screen::bodyHeight() const
{
    const int h = rows() - 3; // title bar + menu bar + column header
    return h > 0 ? h : 0;
}

void Screen::feedInput(const char *data, int len)
{
    if (data && len > 0)
        m_inbuf.append(data, len);
}

int Screen::pollKey()
{
    if (m_inbuf.isEmpty())
        return ERR;

    const auto u = [&](int i) { return static_cast<unsigned char>(m_inbuf[i]); };
    const unsigned char c = u(0);

    // Non-escape byte: return it as-is (handleKey deals with raw bytes like
    // 'q', Tab=9, Enter=10/13, Backspace=8/127).
    if (c != 27) {
        m_inbuf.remove(0, 1);
        return c;
    }

    // ESC: could be a lone Escape key or the start of a CSI/SS3 sequence.
    // Because we decode from a buffer filled per read() burst, a complete
    // sequence normally arrives together; if only ESC is buffered, treat it as
    // the Escape key (matches the previous wgetch behaviour).
    if (m_inbuf.size() == 1) {
        m_inbuf.remove(0, 1);
        return 27;
    }
    const unsigned char b1 = u(1);
    if (b1 != '[' && b1 != 'O') {
        // ESC + some other byte (e.g. Alt-key): surface ESC, keep the rest.
        m_inbuf.remove(0, 1);
        return 27;
    }
    if (m_inbuf.size() < 3)
        return ERR;                    // incomplete CSI/SS3; wait for more bytes
    const unsigned char b2 = u(2);
    switch (b2) {
    case 'A': m_inbuf.remove(0, 3); return KEY_UP;
    case 'B': m_inbuf.remove(0, 3); return KEY_DOWN;
    case 'C': m_inbuf.remove(0, 3); return KEY_RIGHT;
    case 'D': m_inbuf.remove(0, 3); return KEY_LEFT;
    case 'H': m_inbuf.remove(0, 3); return KEY_HOME;
    case 'F': m_inbuf.remove(0, 3); return KEY_END;
    case '5': if (m_inbuf.size() < 4) return ERR; m_inbuf.remove(0, 4); return KEY_PPAGE; // ESC [ 5 ~
    case '6': if (m_inbuf.size() < 4) return ERR; m_inbuf.remove(0, 4); return KEY_NPAGE; // ESC [ 6 ~
    case '1': if (m_inbuf.size() < 4) return ERR; m_inbuf.remove(0, 4); return KEY_HOME;  // ESC [ 1 ~
    case '4': if (m_inbuf.size() < 4) return ERR; m_inbuf.remove(0, 4); return KEY_END;   // ESC [ 4 ~
    default:  m_inbuf.remove(0, 3); return ERR;                                           // unknown; drop
    }
}

void Screen::render(const Frame &f)
{
    if (!m_active)
        return;
    erase();

    const int width = cols();
    const int height = rows();
    if (width < 1 || height < 3) {
        refresh();
        return;
    }

    // Fill an entire chrome line with a role's colour so lines 0 & 1 read as
    // part of the "UI" (a coloured title bar + menu bar) distinct from content.
    const auto fillLine = [&](int y, Role role) {
        attrset(attrFor(role));
        putAt(y, 0, QString(width, QLatin1Char(' ')));
    };

    // --- line 0: title / tab bar (UI chrome) ---
    fillLine(0, Role::TitleBar);
    {
        int x = 0;
        for (int i = 0; i < f.tabs.size(); ++i) {
            const QString label = QStringLiteral(" %1 ").arg(f.tabs[i]);
            // Active tab pops; inactive tabs blend into the title bar.
            attrset(attrFor(i == f.activeTab ? Role::TabActive : Role::TitleBar));
            putAt(0, x, label);
            x += label.size() + 1;
        }
        const QString src = f.sourceLabel;
        const int sx = width - src.size() - 1;
        if (sx > x) {
            attrset(attrFor(Role::TitleBar));
            putAt(0, sx, src);
        }
        attrset(A_NORMAL);
    }

    // --- line 1: menu / key-help bar (UI chrome) ---
    fillLine(1, Role::Footer);
    if (!f.footerHints.isEmpty()) {
        // Structured menu bar: each key glyph pops in the MenuKey colour, its
        // label follows in the Footer colour, items separated by " · ". Stops
        // cleanly at the right edge.
        int x = 1;
        for (int i = 0; i < f.footerHints.size(); ++i) {
            const KeyHint &h = f.footerHints[i];
            if (x + h.key.size() + 1 >= width)
                break;
            attrset(attrFor(Role::MenuKey));
            putAt(1, x, h.key);
            x += h.key.size();
            QString lbl = h.desc.isEmpty() ? QStringLiteral(" ")
                                           : QStringLiteral(" %1").arg(h.desc);
            if (i + 1 < f.footerHints.size())
                lbl += QStringLiteral(" \u00b7 "); // middle dot separator
            if (x + lbl.size() > width)
                lbl = lbl.left(width - x);
            attrset(attrFor(Role::Footer));
            putAt(1, x, lbl);
            x += lbl.size();
        }
        attrset(A_NORMAL);
    } else {
        attrset(attrFor(Role::Footer));
        putAt(1, 0, fitCell(f.footer, width, false));
        attrset(A_NORMAL);
    }

    // --- column widths ---
    const QList<Column> &cols = f.columns;
    const int n = static_cast<int>(cols.size());
    QList<int> w(n, 0);
    int fixedSum = 0;
    for (int c = 1; c < n; ++c) {
        if (cols[c].fixedWidth > 0) {
            w[c] = cols[c].fixedWidth;
        } else {
            int natural = cols[c].title.size();
            for (const QStringList &row : f.rows)
                if (c < row.size())
                    natural = std::max(natural, static_cast<int>(row[c].size()));
            w[c] = std::min(natural, kNumColCap);
        }
        fixedSum += w[c];
    }
    w[0] = width - fixedSum - kSep * (n - 1);
    if (w[0] < 4)
        w[0] = 4;

    const auto rowText = [&](const QStringList &cells) {
        QString line;
        for (int c = 0; c < n; ++c) {
            if (c > 0)
                line += QString(kSep, QLatin1Char(' '));
            const QString cell = c < cells.size() ? cells[c] : QString();
            line += fitCell(cell, w[c], cols[c].rightAlign);
        }
        return line;
    };

    // --- header ---
    {
        QStringList headers;
        for (int c = 0; c < n; ++c) {
            QString h = cols[c].title;
            if (c == f.sortCol)
                h += f.sortDesc ? QStringLiteral(" \u25bc") : QStringLiteral(" \u25b2");
            headers << h;
        }
        attrset(attrFor(Role::Header));
        putLine(2, rowText(headers));
        attrset(A_NORMAL);
    }

    // --- body (starts at line 3: title bar, menu bar, header above) ---
    const int body = bodyHeight();
    const int total = static_cast<int>(f.rows.size());
    const bool showMore = body > 1 && (f.scrollOffset + body) < total;
    const int dataLines = showMore ? body - 1 : body;
    int shown = 0;
    for (int i = 0; i < dataLines && (f.scrollOffset + i) < total; ++i) {
        const int idx = f.scrollOffset + i;
        const Role role = idx < f.rowRoles.size() ? f.rowRoles[idx] : Role::Normal;
        const bool isCursor = (idx == f.cursor);
        attrset(attrFor(role));
        putLine(3 + i, rowText(f.rows[idx]));
        attrset(A_NORMAL);
        const double frac = idx < f.rowGauge.size() ? f.rowGauge[idx] : 0.0;
        if (isCursor) {
            // Current line: keep the bandwidth gauge but mark it bold+underline
            // (don't replace it with a flat bar — the gauge is information).
            const long emph = A_BOLD | A_UNDERLINE;
            const int fill = paintGauge(3 + i, width, frac, role, emph);
            if (fill < width) {
                const short pair = m_hasColor
                    ? static_cast<short>(static_cast<int>(role) + 1) : 0;
                mvchgat(3 + i, fill, width - fill,
                        emph | ncursesAttr(m_theme[role].attr), pair, nullptr);
            }
        } else {
            // Row-spanning bandwidth gauge painted over the text.
            paintGauge(3 + i, width, frac, role);
        }
        ++shown;
    }
    const int below = total - (f.scrollOffset + shown);
    if (showMore && below > 0) {
        attrset(attrFor(Role::Stale));
        putLine(3 + shown,
                fitCell(QStringLiteral("  … +%1 more (↓ to scroll)").arg(below),
                        width, false));
        attrset(A_NORMAL);
    }

    // --- modal panel (drawn last, on top of everything) ---
    if (f.modal.visible)
        renderModal(f.modal);

    refresh();
}

void Screen::renderModal(const ModalPanel &s) const
{
    if (!m_active)
        return;
    const int W = cols();
    const int H = rows();
    if (W < 20 || H < 8)
        return;

    // Widest "label   value" line, plus the help/title/footer, bounds the box.
    int contentW = s.title.size();
    for (const SettingRow &it : s.items)
        contentW = std::max<int>(contentW, it.label.size() + 3 + it.value.size());
    for (const SettingRow &it : s.items)
        contentW = std::max<int>(contentW, it.help.size());
    contentW = std::max<int>(contentW, s.footer.size());

    const int innerW = std::min(W - 4, std::max(40, contentW));
    const int boxW   = innerW + 2;                       // + side borders
    // Inner content rows (between the borders): blank, N items, blank,
    // [help (selectable only)], footer. The title lives in the top border.
    const int helpLines = s.selectable ? 1 : 0;
    const int innerH = 1 + s.items.size() + 1 + helpLines + 1;
    const int boxH   = innerH + 2;
    const int x0 = (W - boxW) / 2;
    const int y0 = std::max(0, (H - boxH) / 2);

    const long border = attrFor(Role::Accent);
    const long normal = attrFor(Role::Normal);
    const long sel    = attrFor(Role::TabActive);
    const long help   = attrFor(Role::Footer);

    const auto borderRow = [&](int y, QChar fill) {
        QString line(boxW, fill);
        attrset(border);
        putAt(y, x0, line);
        attrset(A_NORMAL);
    };
    const auto put = [&](int y, const QString &text, long a, bool center) {
        QString inner = text;
        if (inner.size() > innerW)
            inner = inner.left(innerW - 1) + QChar(0x2026);
        const int pad = innerW - inner.size();
        QString body;
        if (center) {
            const int l = pad / 2;
            body = QString(l, QLatin1Char(' ')) + inner + QString(pad - l, QLatin1Char(' '));
        } else {
            body = inner + QString(pad, QLatin1Char(' '));
        }
        attrset(border);
        mvaddwstr(y, x0, L"\u2502"); // │ left border
        attrset(a);
        putAt(y, x0 + 1, body);
        attrset(border);
        mvaddwstr(y, x0 + 1 + innerW, L"\u2502"); // │ right border
        attrset(A_NORMAL);
    };

    int y = y0;
    // Top border with title.
    {
        QString top = QStringLiteral("\u250c"); // ┌
        QString title = QStringLiteral(" %1 ").arg(s.title);
        int dashes = boxW - 2 - title.size();
        if (dashes < 0) { title = title.left(boxW - 2); dashes = 0; }
        const int left = dashes / 2;
        top += QString(left, QChar(0x2500));
        top += title;
        top += QString(dashes - left, QChar(0x2500));
        top += QStringLiteral("\u2510"); // ┐
        attrset(border);
        putAt(y, x0, top);
        attrset(A_NORMAL);
    }
    ++y;
    put(y++, QString(), normal, false);                   // blank
    for (int i = 0; i < s.items.size(); ++i) {
        const SettingRow &it = s.items[i];
        if (!s.selectable) {
            // Read-only info panel: left-aligned "label   value", no marker,
            // no highlight (label is usually pre-formatted; value optional).
            QString row = QStringLiteral("  ") + it.label;
            if (!it.value.isEmpty()) {
                const int gap = innerW - row.size() - it.value.size();
                if (gap > 0)
                    row += QString(gap, QLatin1Char(' '));
                row += it.value;
            }
            put(y++, row, normal, false);
            continue;
        }
        const bool selected = (i == s.selected);
        const QString marker = selected ? QStringLiteral("\u25b8 ") : QStringLiteral("  "); // ▸
        // "▸ Label" left, "[value]" right.
        const QString value = QStringLiteral("[%1]").arg(it.value);
        QString row = marker + it.label;
        const int gap = innerW - row.size() - value.size();
        if (gap > 0)
            row += QString(gap, QLatin1Char(' '));
        row += value;
        put(y++, row, selected ? sel : normal, false);
    }
    put(y++, QString(), normal, false);                   // blank
    // Help line for the selected item (selectable panels only).
    if (s.selectable) {
        const QString h = (s.selected >= 0 && s.selected < s.items.size())
                               ? s.items[s.selected].help
                               : QString();
        put(y++, h, help, false);
    }
    // Footer key hints.
    put(y++, s.footer, help, false);
    // Bottom border.
    borderRow(y, QChar(0x2500));
    // Redraw bottom corners over the filled line.
    attrset(border);
    mvaddwstr(y, x0, L"\u2514");                 // └
    mvaddwstr(y, x0 + boxW - 1, L"\u2518");      // ┘
    attrset(A_NORMAL);
}

} // namespace qiftop::tui
