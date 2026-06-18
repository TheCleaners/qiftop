#include "GuiTheme.h"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

namespace qiftop::ui {

namespace {

// Raw colour inputs for a theme; buildPalette() expands them into a full
// QPalette (every role we care about, plus the Disabled group). Keeping the
// inputs flat makes each theme below a readable block of hex.
struct ThemeColors {
    QColor window;       // Window / dialog background
    QColor windowText;   // primary text on Window
    QColor base;         // text-entry / item-view background
    QColor alternate;    // alternating row background
    QColor text;         // text on Base
    QColor button;       // button face
    QColor buttonText;   // button label
    QColor brightText;   // "bright" role (rare; used for emphasis)
    QColor highlight;    // selection background
    QColor highlightText;// selection text
    QColor tooltipBase;  // tooltip background
    QColor tooltipText;  // tooltip text
    QColor placeholder;  // muted/placeholder text (proto tag, separators)
    QColor link;         // hyperlink
    QColor disabledText; // greyed text in the Disabled group
};

QPalette buildPalette(const ThemeColors &c)
{
    QPalette p;
    p.setColor(QPalette::Window,          c.window);
    p.setColor(QPalette::WindowText,      c.windowText);
    p.setColor(QPalette::Base,            c.base);
    p.setColor(QPalette::AlternateBase,   c.alternate);
    p.setColor(QPalette::Text,            c.text);
    p.setColor(QPalette::Button,          c.button);
    p.setColor(QPalette::ButtonText,      c.buttonText);
    p.setColor(QPalette::BrightText,      c.brightText);
    p.setColor(QPalette::Highlight,       c.highlight);
    p.setColor(QPalette::HighlightedText, c.highlightText);
    p.setColor(QPalette::ToolTipBase,     c.tooltipBase);
    p.setColor(QPalette::ToolTipText,     c.tooltipText);
    p.setColor(QPalette::PlaceholderText, c.placeholder);
    p.setColor(QPalette::Link,            c.link);

    // Derive a couple of secondary roles from the inputs so frames/3D edges
    // and the disabled "window" read sensibly without hand-tuning each theme.
    p.setColor(QPalette::Light,    c.button.lighter(140));
    p.setColor(QPalette::Midlight, c.button.lighter(115));
    p.setColor(QPalette::Mid,      c.button.darker(130));
    p.setColor(QPalette::Dark,     c.button.darker(160));
    p.setColor(QPalette::Shadow,   c.button.darker(220));

    // Disabled group: greyed text everywhere a widget can be disabled, so
    // inactive menu items / checkboxes don't render as crisp full-contrast
    // text on a themed palette.
    p.setColor(QPalette::Disabled, QPalette::Text,            c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::WindowText,      c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText,      c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight,       c.highlight.darker(150));
    p.setColor(QPalette::Disabled, QPalette::Base,            c.base);
    p.setColor(QPalette::Disabled, QPalette::Window,          c.window);
    p.setColor(QPalette::Disabled, QPalette::PlaceholderText, c.disabledText);
    return p;
}

// One captured snapshot of the native look so "System" can restore it. Held
// behind a function-local static (constructed on first use) rather than a
// namespace-scope QPalette — the latter trips clang-tidy's
// bugprone-throwing-static-initialization because QPalette's ctor can throw
// during static init. The public API can be called before a QApplication
// exists (records empty defaults then).
struct SystemThemeSnapshot {
    bool     captured = false;
    QString  styleName;
    QPalette palette;
};

SystemThemeSnapshot &systemSnapshot()
{
    static SystemThemeSnapshot s;
    return s;
}

} // namespace

QList<GuiTheme> builtinGuiThemes()
{
    QList<GuiTheme> themes;

    // 1) System — followSystem; palette/accents are placeholders never used
    //    while followSystem is true (the delegate falls back to its own
    //    computed dark/light accents in this mode).
    {
        GuiTheme t;
        t.name = QStringLiteral("System");
        t.followSystem = true;
        themes.push_back(t);
    }

    // 2) Dark — neutral charcoal. Accents match the delegate's historical
    //    dark defaults so a "Dark"-themed build reads like today's dark
    //    system theme.
    {
        ThemeColors c;
        c.window = QColor(0x2b2b2b); c.windowText = QColor(0xe6e6e6);
        c.base = QColor(0x1e1e1e);   c.alternate = QColor(0x262626);
        c.text = QColor(0xe6e6e6);   c.button = QColor(0x353535);
        c.buttonText = QColor(0xe6e6e6); c.brightText = QColor(0xffffff);
        c.highlight = QColor(0x3d6fb5); c.highlightText = QColor(0xffffff);
        c.tooltipBase = QColor(0x353535); c.tooltipText = QColor(0xe6e6e6);
        c.placeholder = QColor(0x9aa0a6); c.link = QColor(0x6cb6ff);
        c.disabledText = QColor(0x6b6b6b);
        themes.push_back({QStringLiteral("Dark"), false, buildPalette(c),
                          QColor(0x6CB6FF), QColor(0xF0B86E)});
    }

    // 3) Light — clean off-white. Accents = the delegate's historical light
    //    defaults.
    {
        ThemeColors c;
        c.window = QColor(0xf0f0f0); c.windowText = QColor(0x202020);
        c.base = QColor(0xffffff);   c.alternate = QColor(0xf5f5f5);
        c.text = QColor(0x202020);   c.button = QColor(0xe6e6e6);
        c.buttonText = QColor(0x202020); c.brightText = QColor(0xffffff);
        c.highlight = QColor(0x2d6fb5); c.highlightText = QColor(0xffffff);
        c.tooltipBase = QColor(0xffffdc); c.tooltipText = QColor(0x202020);
        c.placeholder = QColor(0x6b7178); c.link = QColor(0x0b5fa5);
        c.disabledText = QColor(0xa0a0a0);
        themes.push_back({QStringLiteral("Light"), false, buildPalette(c),
                          QColor(0x0B5FA5), QColor(0xA0521B)});
    }

    // 4) Nord — arctic, bluish dark (nordtheme.com). Polar Night bg, Snow
    //    Storm text, Frost accents.
    {
        ThemeColors c;
        c.window = QColor(0x2e3440); c.windowText = QColor(0xeceff4);
        c.base = QColor(0x272c36);   c.alternate = QColor(0x3b4252);
        c.text = QColor(0xe5e9f0);   c.button = QColor(0x3b4252);
        c.buttonText = QColor(0xeceff4); c.brightText = QColor(0xffffff);
        c.highlight = QColor(0x5e81ac); c.highlightText = QColor(0xeceff4);
        c.tooltipBase = QColor(0x3b4252); c.tooltipText = QColor(0xeceff4);
        c.placeholder = QColor(0x8a93a5); c.link = QColor(0x88c0d0);
        c.disabledText = QColor(0x616a7d);
        themes.push_back({QStringLiteral("Nord"), false, buildPalette(c),
                          QColor(0x88C0D0), QColor(0xD08770)});
    }

    // 5) Solarized Dark (ethanschoonover.com/solarized).
    {
        ThemeColors c;
        c.window = QColor(0x002b36); c.windowText = QColor(0x93a1a1);
        c.base = QColor(0x073642);   c.alternate = QColor(0x0a3a47);
        c.text = QColor(0x93a1a1);   c.button = QColor(0x073642);
        c.buttonText = QColor(0x93a1a1); c.brightText = QColor(0xfdf6e3);
        c.highlight = QColor(0x268bd2); c.highlightText = QColor(0xfdf6e3);
        c.tooltipBase = QColor(0x073642); c.tooltipText = QColor(0x93a1a1);
        c.placeholder = QColor(0x586e75); c.link = QColor(0x268bd2);
        c.disabledText = QColor(0x4a5d63);
        themes.push_back({QStringLiteral("Solarized Dark"), false, buildPalette(c),
                          QColor(0x268BD2), QColor(0xCB4B16)});
    }

    // 6) Solarized Light.
    {
        ThemeColors c;
        c.window = QColor(0xeee8d5); c.windowText = QColor(0x586e75);
        c.base = QColor(0xfdf6e3);   c.alternate = QColor(0xeee8d5);
        c.text = QColor(0x586e75);   c.button = QColor(0xeee8d5);
        c.buttonText = QColor(0x586e75); c.brightText = QColor(0x002b36);
        c.highlight = QColor(0x268bd2); c.highlightText = QColor(0xfdf6e3);
        c.tooltipBase = QColor(0xfdf6e3); c.tooltipText = QColor(0x586e75);
        c.placeholder = QColor(0x93a1a1); c.link = QColor(0x268bd2);
        c.disabledText = QColor(0xb5ad95);
        themes.push_back({QStringLiteral("Solarized Light"), false, buildPalette(c),
                          QColor(0x268BD2), QColor(0xCB4B16)});
    }

    // 7) Gruvbox Dark (github.com/morhetz/gruvbox).
    {
        ThemeColors c;
        c.window = QColor(0x282828); c.windowText = QColor(0xebdbb2);
        c.base = QColor(0x1d2021);   c.alternate = QColor(0x32302f);
        c.text = QColor(0xebdbb2);   c.button = QColor(0x3c3836);
        c.buttonText = QColor(0xebdbb2); c.brightText = QColor(0xfbf1c7);
        c.highlight = QColor(0x458588); c.highlightText = QColor(0xfbf1c7);
        c.tooltipBase = QColor(0x3c3836); c.tooltipText = QColor(0xebdbb2);
        c.placeholder = QColor(0x928374); c.link = QColor(0x83a598);
        c.disabledText = QColor(0x665c54);
        themes.push_back({QStringLiteral("Gruvbox Dark"), false, buildPalette(c),
                          QColor(0x83A598), QColor(0xFE8019)});
    }

    return themes;
}

int guiThemeIndexByName(const QList<GuiTheme> &themes, const QString &name)
{
    for (int i = 0; i < themes.size(); ++i) {
        if (themes.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

void captureSystemTheme()
{
    auto &snap = systemSnapshot();
    if (snap.captured) return;
    snap.captured = true;
    if (auto *style = QApplication::style())
        snap.styleName = style->objectName();
    snap.palette = QApplication::palette();
}

void applyGuiTheme(const GuiTheme &theme)
{
    // Make sure we have a baseline to restore to even if the caller forgot
    // to capture explicitly — first apply doubles as the capture point.
    auto &snap = systemSnapshot();
    if (!snap.captured) captureSystemTheme();

    if (theme.followSystem) {
        if (!snap.styleName.isEmpty()) {
            if (QStyle *s = QStyleFactory::create(snap.styleName))
                QApplication::setStyle(s);
        }
        QApplication::setPalette(snap.palette);
        return;
    }

    // Fusion is the one widely-available style that fully honours an
    // arbitrary QPalette; native styles paint their own colours and would
    // ignore most of the theme.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        QApplication::setStyle(fusion);
    QApplication::setPalette(theme.palette);
}

} // namespace qiftop::ui
