// Unit tests for the nqiftop theme model (src/tui/TuiTheme.h). Pure data — no
// ncurses, no aggregators.

#include <QSet>
#include <QTest>

#include "tui/TuiTheme.h"

using namespace qiftop::tui;

class TestTuiTheme : public QObject {
    Q_OBJECT

private slots:
    void shipsTheExpectedVariants()
    {
        const QStringList names = themeNames();
        QCOMPARE(names.size(), 4);
        QVERIFY(names.contains(QStringLiteral("dark")));
        QVERIFY(names.contains(QStringLiteral("light")));
        QVERIFY(names.contains(QStringLiteral("colorblind")));
        QVERIFY(names.contains(QStringLiteral("mono")));
    }

    void themeByNameMatchesCaseInsensitively()
    {
        bool found = false;
        const Theme t = themeByName(QStringLiteral("LIGHT"), &found);
        QVERIFY(found);
        QCOMPARE(t.name, QStringLiteral("light"));
    }

    void unknownThemeFallsBackToFirst()
    {
        bool found = true;
        const Theme t = themeByName(QStringLiteral("nonesuch"), &found);
        QVERIFY(!found);
        QCOMPARE(t.name, builtinThemes().first().name); // dark
    }

    void colourThemesDistinguishDirections()
    {
        // For the colour themes the three flow directions must use distinct
        // foregrounds so they're visually separable.
        for (const Theme &t : builtinThemes()) {
            if (t.name == QStringLiteral("mono"))
                continue;
            QSet<int> fgs{t[Role::Outbound].fg, t[Role::Inbound].fg, t[Role::Forwarded].fg};
            QCOMPARE(fgs.size(), 3); // all different
        }
    }

    void monoThemeDistinguishesByAttr()
    {
        // mono has no colour, so direction must be separable by attributes.
        const Theme m = themeByName(QStringLiteral("mono"));
        QCOMPARE(m[Role::Outbound].fg, color::Default);
        QSet<int> attrs{m[Role::Outbound].attr, m[Role::Inbound].attr, m[Role::Forwarded].attr};
        QCOMPARE(attrs.size(), 3);
    }
};

QTEST_APPLESS_MAIN(TestTuiTheme)
#include "test_tui_theme.moc"
