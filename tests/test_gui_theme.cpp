// Unit tests for the GUI theme model (src/ui/GuiTheme.h). Pure data: builds
// QPalettes headlessly under QGuiApplication (offscreen). No window, no apply
// — applyGuiTheme()'s QApplication/style plumbing isn't exercised here.

#include <QSet>
#include <QtTest/QtTest>

#include "ui/GuiTheme.h"

using namespace qiftop::ui;

class TestGuiTheme : public QObject {
    Q_OBJECT

private slots:
    void shipsSystemFirstPlusNamedSet()
    {
        const auto themes = builtinGuiThemes();
        QVERIFY(themes.size() >= 4);
        // System is always first and is the only followSystem theme.
        QCOMPARE(themes.first().name, QStringLiteral("System"));
        QVERIFY(themes.first().followSystem);

        QStringList names;
        for (const auto &t : themes) names << t.name;
        QVERIFY(names.contains(QStringLiteral("Dark")));
        QVERIFY(names.contains(QStringLiteral("Light")));
        QVERIFY(names.contains(QStringLiteral("Nord")));
    }

    void onlySystemFollowsSystem()
    {
        const auto themes = builtinGuiThemes();
        int followCount = 0;
        for (const auto &t : themes) {
            if (t.followSystem) {
                ++followCount;
                QCOMPARE(t.name, QStringLiteral("System"));
            }
        }
        QCOMPARE(followCount, 1);
    }

    void lookupIsCaseInsensitive()
    {
        const auto themes = builtinGuiThemes();
        const int dark = guiThemeIndexByName(themes, QStringLiteral("dark"));
        QVERIFY(dark > 0);
        QCOMPARE(themes.at(dark).name, QStringLiteral("Dark"));

        QCOMPARE(guiThemeIndexByName(themes, QStringLiteral("SYSTEM")), 0);
        QCOMPARE(guiThemeIndexByName(themes, QStringLiteral("NoRd")),
                 guiThemeIndexByName(themes, QStringLiteral("Nord")));
    }

    void lookupUnknownReturnsMinusOne()
    {
        const auto themes = builtinGuiThemes();
        QCOMPARE(guiThemeIndexByName(themes, QStringLiteral("nonesuch")), -1);
        QCOMPARE(guiThemeIndexByName(themes, QString()), -1);
    }

    void namedThemesHaveValidDistinctAccents()
    {
        const auto themes = builtinGuiThemes();
        for (const auto &t : themes) {
            if (t.followSystem)
                continue;
            // Real themes carry valid, non-identical flow accents.
            QVERIFY2(t.flowSource.isValid(), qPrintable(t.name));
            QVERIFY2(t.flowDest.isValid(), qPrintable(t.name));
            QVERIFY2(t.flowSource != t.flowDest, qPrintable(t.name));

            // And a real palette: Base and WindowText must differ (otherwise
            // text would be invisible), and the palette must not be the
            // default-constructed one.
            const QColor base = t.palette.color(QPalette::Base);
            const QColor text = t.palette.color(QPalette::Text);
            QVERIFY2(base.isValid() && text.isValid(), qPrintable(t.name));
            QVERIFY2(base != text, qPrintable(t.name));
        }
    }

    void systemThemeLeavesAccentsForDelegateDefault()
    {
        // The System theme intentionally ships invalid accents — the delegate
        // detects this and keeps its luminance-derived dark/light defaults.
        const auto themes = builtinGuiThemes();
        QVERIFY(!themes.first().flowSource.isValid());
        QVERIFY(!themes.first().flowDest.isValid());
    }

    void darkAndLightDifferInBaseLuminance()
    {
        const auto themes = builtinGuiThemes();
        const int di = guiThemeIndexByName(themes, QStringLiteral("Dark"));
        const int li = guiThemeIndexByName(themes, QStringLiteral("Light"));
        QVERIFY(di > 0 && li > 0);
        const int darkL  = themes.at(di).palette.color(QPalette::Base).lightness();
        const int lightL = themes.at(li).palette.color(QPalette::Base).lightness();
        QVERIFY(darkL < 128);
        QVERIFY(lightL >= 128);
    }
};

QTEST_MAIN(TestGuiTheme)
#include "test_gui_theme.moc"
