#include "tui/Screen.h"

#include <clocale>

// ncurses last: it defines lower-case macros (erase, move, refresh, timeout…)
// that would clash with Qt/STL identifiers if included before them.
#include <ncurses.h>

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

void putLine(int y, const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    mvaddstr(y, 0, utf8.constData());
}

} // namespace

Screen::~Screen() { shutdown(); }

void Screen::init()
{
    if (m_active)
        return;
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0);
    m_active = true;
}

void Screen::shutdown()
{
    if (!m_active)
        return;
    endwin();
    m_active = false;
}

int Screen::rows() const
{
    return m_active ? getmaxy(stdscr) : 0;
}

int Screen::cols() const
{
    return m_active ? getmaxx(stdscr) : 0;
}

int Screen::bodyHeight() const
{
    // chrome: tab line (0), column header (1), footer (last) => 3 lines.
    const int h = rows() - 3;
    return h > 0 ? h : 0;
}

int Screen::pollKey()
{
    return m_active ? wgetch(stdscr) : ERR;
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

    // --- tab line ---
    {
        int x = 0;
        for (int i = 0; i < f.tabs.size(); ++i) {
            const QString label = QStringLiteral(" %1 ").arg(f.tabs[i]);
            const QByteArray u = label.toUtf8();
            if (i == f.activeTab) attron(A_REVERSE);
            mvaddstr(0, x, u.constData());
            if (i == f.activeTab) attroff(A_REVERSE);
            x += label.size() + 1;
        }
        // source label, right-aligned
        const QString src = f.sourceLabel;
        const int sx = width - src.size() - 1;
        if (sx > x)
            mvaddstr(0, sx, src.toUtf8().constData());
    }

    // --- column widths ---
    const QList<Column> &cols = f.columns;
    const int n = static_cast<int>(cols.size());
    QList<int> w(n, 0);
    int fixedSum = 0;
    for (int c = 1; c < n; ++c) {
        int natural = cols[c].title.size();
        for (const QStringList &row : f.rows)
            if (c < row.size())
                natural = std::max(natural, static_cast<int>(row[c].size()));
        w[c] = std::min(natural, kNumColCap);
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
                h += f.sortDesc ? QStringLiteral(" v") : QStringLiteral(" ^");
            headers << h;
        }
        attron(A_BOLD | A_UNDERLINE);
        putLine(1, rowText(headers));
        attroff(A_BOLD | A_UNDERLINE);
    }

    // --- body ---
    const int body = bodyHeight();
    const int total = static_cast<int>(f.rows.size());
    int shown = 0;
    for (int i = 0; i < body && (f.scrollOffset + i) < total; ++i) {
        putLine(2 + i, rowText(f.rows[f.scrollOffset + i]));
        ++shown;
    }
    // "+N more" hint when the list overflows below the fold.
    const int below = total - (f.scrollOffset + shown);
    if (below > 0 && body > 0) {
        attron(A_DIM);
        putLine(2 + body - 1,
                QStringLiteral("  … +%1 more (↓ to scroll)").arg(below));
        attroff(A_DIM);
    }

    // --- footer ---
    {
        attron(A_REVERSE);
        const QString f2 = fitCell(f.footer, width, false);
        putLine(height - 1, f2);
        attroff(A_REVERSE);
    }

    refresh();
}

} // namespace qiftop::tui
