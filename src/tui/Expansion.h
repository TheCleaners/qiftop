#pragma once

// Reusable expand/collapse state for a keyed, re-sortable list — the
// aptitude-style inline "tree" pattern, isolated from any particular view.
//
// Expansion is tracked by STABLE IDENTITY KEY (e.g. an interface name or a
// connection 5-tuple), not by row index, so an entry stays expanded across
// re-sorts, insertions and removals of the underlying list.
//
// nqiftop's live Interfaces / Connections detail uses the Detail *modal
// overlay* instead of inline expansion (a live, rate-sorted list reflowing
// under an inline subtree is unusable). This component is kept deliberately
// for views where inline expansion IS the right fit — e.g. a future
// hierarchical settings tree or grouped drill-downs — so the pattern (and its
// tests) live on without coupling to the network views.
//
// Pure data + Qt containers; no ncurses, no model/view. Unit-tested.

#include <QSet>
#include <QString>

namespace qiftop::tui {

class ExpansionState {
public:
    [[nodiscard]] bool isExpanded(const QString &key) const
    {
        return m_open.contains(key);
    }

    void expand(const QString &key)   { m_open.insert(key); }
    void collapse(const QString &key) { m_open.remove(key); }

    // Flip the state; returns the new state (true == now expanded).
    bool toggle(const QString &key)
    {
        if (m_open.remove(key))
            return false;
        m_open.insert(key);
        return true;
    }

    // dir > 0 expand, dir < 0 collapse, dir == 0 toggle. Returns new state.
    bool apply(const QString &key, int dir)
    {
        if (dir > 0)      { m_open.insert(key); return true; }
        if (dir < 0)      { m_open.remove(key); return false; }
        return toggle(key);
    }

    void clear() { m_open.clear(); }
    [[nodiscard]] int  count() const { return static_cast<int>(m_open.size()); }
    [[nodiscard]] bool isEmpty() const { return m_open.isEmpty(); }

    // Drop keys that are no longer present in `liveKeys` so the set can't grow
    // without bound as entries come and go over a long-running session.
    void retainOnly(const QSet<QString> &liveKeys)
    {
        m_open.intersect(liveKeys);
    }

private:
    QSet<QString> m_open;
};

} // namespace qiftop::tui
