#include <QtTest>

#include <QImage>
#include <QPainter>

#include "ui/Theme.h"

using namespace oqb::ui;

/// The window's palette, stylesheet and painted icons.
///
/// The icons are drawn rather than loaded, so a mistake in the geometry or the
/// stylesheet shows up as a blank control rather than a missing file. These
/// cases make that failure loud: every icon must paint something, and both
/// themes must produce a stylesheet that mentions the colours they define.
class ThemeTest : public QObject
{
    Q_OBJECT

private slots:
    void bothThemesFillEveryColour();
    void stylesheetIsNotEmptyAndThemed();
    void everyIconPaintsSomething();
    void iconsFollowTheRequestedColour();
};

void ThemeTest::bothThemesFillEveryColour()
{
    for (const Theme &theme : {Theme::light(), Theme::dark()}) {
        QVERIFY2(theme.windowBackground.isValid(), "window background");
        QVERIFY2(theme.chromeBackground.isValid(), "chrome background");
        QVERIFY2(theme.chromeBorder.isValid(), "chrome border");
        QVERIFY2(theme.tabText.isValid(), "tab text");
        QVERIFY2(theme.tabTextActive.isValid(), "active tab text");
        QVERIFY2(theme.tabHover.isValid(), "tab hover");
        QVERIFY2(theme.tabActive.isValid(), "active tab");
        QVERIFY2(theme.iconColor.isValid(), "icon colour");
        QVERIFY2(theme.fieldBackground.isValid(), "field background");
        QVERIFY2(theme.fieldText.isValid(), "field text");
        QVERIFY2(theme.accent.isValid(), "accent");
        QVERIFY2(theme.statusText.isValid(), "status text");
    }

    // The two themes have to differ, or one of them was not built.
    QVERIFY(Theme::light().chromeBackground != Theme::dark().chromeBackground);
}

void ThemeTest::stylesheetIsNotEmptyAndThemed()
{
    for (const Theme &theme : {Theme::light(), Theme::dark()}) {
        const QString style = theme.styleSheet();
        QVERIFY2(style.size() > 200, "the stylesheet is suspiciously short");

        // The colours are substituted into the sheet, so a placeholder left
        // behind means a colour is missing and Qt would silently ignore the
        // rule that contains it.
        QVERIFY2(!style.contains(QStringLiteral("%(")),
                 "a colour placeholder was not substituted");
        QVERIFY2(!style.contains(QStringLiteral(")s")),
                 "a colour placeholder was not substituted");

        // The parts of the window the theme is responsible for.
        for (const char *selector : {"QTabBar::tab", "#AddressBar", "#LoadProgress",
                                     "QStatusBar", "QMenu"}) {
            QVERIFY2(style.contains(QLatin1String(selector)), selector);
        }
    }
}

void ThemeTest::everyIconPaintsSomething()
{
    const Theme theme = Theme::light();

    const QList<NavIcon> icons = {
        NavIcon::Back,      NavIcon::Forward,   NavIcon::Reload,  NavIcon::Stop,
        NavIcon::Home,      NavIcon::NewTab,    NavIcon::Star,    NavIcon::StarFilled,
        NavIcon::Menu,      NavIcon::History,   NavIcon::Bookmarks, NavIcon::Info,
        NavIcon::Lock,      NavIcon::Globe,     NavIcon::Close,
    };

    for (NavIcon which : icons) {
        const QIcon icon = theme.icon(which, QColor(0, 0, 0), 18);
        QVERIFY2(!icon.isNull(), "an icon painted nothing at all");

        const QImage image = icon.pixmap(18, 18).toImage();
        QVERIFY(!image.isNull());

        // A blank icon would leave every pixel transparent, which is what a
        // geometry mistake or a missing case produces.
        bool painted = false;
        for (int y = 0; y < image.height() && !painted; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() > 0) {
                    painted = true;
                    break;
                }
            }
        }
        QVERIFY2(painted, "an icon painted only transparent pixels");
    }
}

void ThemeTest::iconsFollowTheRequestedColour()
{
    const Theme theme = Theme::light();

    // The same geometry in two colours has to actually differ, or a disabled
    // control would look exactly like an enabled one.
    const QImage red = theme.icon(NavIcon::Back, QColor(255, 0, 0), 18)
                           .pixmap(18, 18)
                           .toImage();
    const QImage blue = theme.icon(NavIcon::Back, QColor(0, 0, 255), 18)
                            .pixmap(18, 18)
                            .toImage();

    QVERIFY(red != blue);

    // The requested colour is what lands on the canvas.
    bool sawRed = false;
    for (int y = 0; y < red.height() && !sawRed; ++y) {
        for (int x = 0; x < red.width(); ++x) {
            const QColor pixel = red.pixelColor(x, y);
            if (pixel.alpha() > 200 && pixel.red() > 200 && pixel.green() < 60
                && pixel.blue() < 60) {
                sawRed = true;
                break;
            }
        }
    }
    QVERIFY2(sawRed, "the icon was not painted in the colour asked for");
}

QTEST_MAIN(ThemeTest)
#include "tst_theme.moc"
