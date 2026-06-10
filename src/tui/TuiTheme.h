#pragma once

// Theme model for nqiftop: a mapping from semantic UI roles to colour + attr.
// Deliberately ncurses-free (plain ints) so it's unit-testable and so a
// notcurses backend could consume the same themes. Screen translates the
// role colours into init_pair()s and the attr bits into A_* attributes.

#include <QList>
#include <QString>

#include <initializer_list>
#include <utility>

namespace qiftop::tui {

// Semantic roles a renderer colours. Connection rows pick Outbound/Inbound/
// Forwarded/Stale; everything else is chrome.
enum class Role {
    Normal,        // default body text
    Header,        // column header row
    TabActive,     // selected tab
    TabInactive,   // other tabs
    Footer,        // key-help line
    SortIndicator, // the sorted column's header
    Outbound,      // direction tint: local -> remote
    Inbound,       // direction tint: remote -> local
    Forwarded,     // routed THROUGH this host (neither end is us)
    Stale,         // flow absent from the latest tick
    Accent,        // source label, hints
    Count
};

// ncurses colour numbers (COLOR_* == 0..7); -1 == terminal default.
namespace color {
constexpr int Default = -1;
constexpr int Black   = 0;
constexpr int Red     = 1;
constexpr int Green   = 2;
constexpr int Yellow  = 3;
constexpr int Blue    = 4;
constexpr int Magenta = 5;
constexpr int Cyan    = 6;
constexpr int White   = 7;
} // namespace color

// Backend-agnostic attribute bits (Screen maps to A_BOLD/A_DIM/...).
namespace attr {
constexpr int None      = 0;
constexpr int Bold      = 1 << 0;
constexpr int Dim       = 1 << 1;
constexpr int Reverse   = 1 << 2;
constexpr int Underline = 1 << 3;
} // namespace attr

struct ThemeColor {
    int fg   = color::Default;
    int bg   = color::Default;
    int attr = attr::None;
};

struct Theme {
    QString     name;
    ThemeColor  roles[static_cast<int>(Role::Count)];
    // Background colour for the row-spanning bandwidth gauge fill. Used only
    // on 256-colour terminals (a subtle tint behind the text, like the Qt
    // RowGaugeDelegate); -1 means "no colour" and Screen falls back to a
    // reverse-video fill so the gauge still reads on 8-colour / mono.
    int         gaugeBg = -1;

    [[nodiscard]] const ThemeColor &operator[](Role r) const
    {
        return roles[static_cast<int>(r)];
    }
};

inline Theme makeTheme(QString name,
                       std::initializer_list<std::pair<Role, ThemeColor>> entries)
{
    Theme t;
    t.name = std::move(name);
    for (const auto &e : entries)
        t.roles[static_cast<int>(e.first)] = e.second;
    return t;
}

// --- the shipped variants ---------------------------------------------------
// Each colours the three flow directions distinctly (the qiftop colour-coding
// semantics: outbound/inbound/forwarded) plus the chrome. `mono` uses only
// attributes so it works on colour-less terminals; `colorblind` uses a
// cyan/magenta/yellow triad that stays distinguishable under common CVD.

inline QList<Theme> builtinThemes()
{
    using namespace color;
    using namespace attr;
    QList<Theme> all = {
        makeTheme(QStringLiteral("dark"), {
            {Role::Header,        {Cyan,    Default, Bold}},
            {Role::TabActive,     {Default, Default, Reverse | Bold}},
            {Role::TabInactive,   {Default, Default, Dim}},
            {Role::Footer,        {Default, Default, Reverse}},
            {Role::SortIndicator, {Yellow,  Default, Bold}},
            {Role::Outbound,      {Green,   Default, None}},
            {Role::Inbound,       {Red,     Default, None}},
            {Role::Forwarded,     {Yellow,  Default, None}},
            {Role::Stale,         {Default, Default, Dim}},
            {Role::Accent,        {Blue,    Default, Bold}},
        }),
        makeTheme(QStringLiteral("light"), {
            {Role::Header,        {Blue,    Default, Bold}},
            {Role::TabActive,     {Default, Default, Reverse | Bold}},
            {Role::TabInactive,   {Default, Default, Dim}},
            {Role::Footer,        {Default, Default, Reverse}},
            {Role::SortIndicator, {Magenta, Default, Bold}},
            {Role::Outbound,      {Green,   Default, Bold}},
            {Role::Inbound,       {Red,     Default, Bold}},
            {Role::Forwarded,     {Magenta, Default, None}},
            {Role::Stale,         {Default, Default, Dim}},
            {Role::Accent,        {Blue,    Default, Bold}},
        }),
        makeTheme(QStringLiteral("colorblind"), {
            {Role::Header,        {Cyan,    Default, Bold}},
            {Role::TabActive,     {Default, Default, Reverse | Bold}},
            {Role::TabInactive,   {Default, Default, Dim}},
            {Role::Footer,        {Default, Default, Reverse}},
            {Role::SortIndicator, {White,   Default, Bold}},
            {Role::Outbound,      {Cyan,    Default, None}},
            {Role::Inbound,       {Magenta, Default, None}},
            {Role::Forwarded,     {Yellow,  Default, None}},
            {Role::Stale,         {Default, Default, Dim}},
            {Role::Accent,        {Cyan,    Default, Bold}},
        }),
        makeTheme(QStringLiteral("mono"), {
            {Role::Header,        {Default, Default, Bold | Underline}},
            {Role::TabActive,     {Default, Default, Reverse | Bold}},
            {Role::TabInactive,   {Default, Default, Dim}},
            {Role::Footer,        {Default, Default, Reverse}},
            {Role::SortIndicator, {Default, Default, Bold}},
            {Role::Outbound,      {Default, Default, None}},
            {Role::Inbound,       {Default, Default, Bold}},
            {Role::Forwarded,     {Default, Default, Underline}},
            {Role::Stale,         {Default, Default, Dim}},
            {Role::Accent,        {Default, Default, Bold}},
        }),
    };
    // Row-spanning gauge fill tint (256-colour only; -1 -> reverse fallback).
    all[0].gaugeBg = 236; // dark      : very dark gray
    all[1].gaugeBg = 252; // light     : light gray
    all[2].gaugeBg = 236; // colorblind: very dark gray
    all[3].gaugeBg = -1;  // mono      : reverse-video fill
    return all;
}

inline QStringList themeNames()
{
    QStringList names;
    for (const Theme &t : builtinThemes())
        names << t.name;
    return names;
}

// Theme by name; falls back to the first (dark) when unknown. `found` (when
// non-null) reports whether the name matched.
inline Theme themeByName(const QString &name, bool *found = nullptr)
{
    const QList<Theme> all = builtinThemes();
    for (const Theme &t : all) {
        if (t.name.compare(name, Qt::CaseInsensitive) == 0) {
            if (found) *found = true;
            return t;
        }
    }
    if (found) *found = false;
    return all.first();
}

} // namespace qiftop::tui
