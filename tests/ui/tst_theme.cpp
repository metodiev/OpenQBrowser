#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QPainterPath>

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
    void aDisabledIconKeepsTheColourItWasGiven();
    void backAndForwardAreSolidTriangles();
    void noIconIsDrawnOversizedOrClipped();
    /// Writes a sheet of every icon for review by eye. Skipped unless
    /// OQB_ICON_PREVIEW names a file.
    void writeIconSheet();
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

void ThemeTest::aDisabledIconKeepsTheColourItWasGiven()
{
    // Qt fades an icon itself when its widget is disabled, and that fade is
    // applied on top of whatever colour the icon holds. An icon built with a
    // colour chosen to read as "unavailable" therefore came out almost
    // invisible. Supplying a pixmap for QIcon::Disabled is what stops Qt from
    // deriving its own, so this asserts the colour survives.
    const Theme theme = Theme::light();

    const QIcon paired = theme.icon(NavIcon::Back, theme.iconColor,
                                    theme.iconColorDisabled, 18);

    const QImage normal = paired.pixmap(18, 18, QIcon::Normal).toImage();
    const QImage disabled = paired.pixmap(18, 18, QIcon::Disabled).toImage();
    QVERIFY(!normal.isNull());
    QVERIFY(!disabled.isNull());

    const auto darkest = [](const QImage &image) {
        int best = 256;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() < 250)
                    continue;
                best = qMin(best, (pixel.red() + pixel.green() + pixel.blue()) / 3);
            }
        }
        return best;
    };

    const int normalLuma = darkest(normal);
    const int disabledLuma = darkest(disabled);

    // The disabled rendering is the colour that was asked for, not a faded
    // version of it. The comparison allows a step of rounding, because a pixmap
    // stores alpha premultiplied and unpremultiplying can lose the lowest bit.
    const int expected = (theme.iconColorDisabled.red() + theme.iconColorDisabled.green()
                          + theme.iconColorDisabled.blue())
        / 3;
    QVERIFY2(qAbs(disabledLuma - expected) <= 2,
             qPrintable(QStringLiteral("disabled luma %1, expected about %2")
                            .arg(disabledLuma)
                            .arg(expected)));

    // And it is clearly distinguishable from the enabled one, while still being
    // far enough from a white toolbar to read as a drawn arrow rather than as
    // empty space.
    QVERIFY2(disabledLuma > normalLuma + 40, "a disabled icon must look different");
    QVERIFY2(disabledLuma < 205, "a disabled icon must still be visible");
}

void ThemeTest::backAndForwardAreSolidTriangles()
{
    // Back and forward are drawn as filled triangles rather than as strokes, so
    // that the shape survives being rendered at 16 pixels - a thin chevron loses
    // its silhouette to antialiasing. This measures how much of the glyph is
    // ink, which is what tells a filled shape from an outlined one.
    const Theme theme = Theme::light();

    // The widest unbroken run of ink on any scanline, as a fraction of the
    // glyph's width.
    //
    // A filled triangle has a wide run where its base is; a chevron drawn with a
    // stroke never runs wider than the pen, because its two arms only meet at a
    // point. That difference - not the total amount of ink - is what tells a
    // solid shape from an outline, and it does not depend on where in the
    // pixmap the glyph sits.
    const auto widestRun = [&](NavIcon which) {
        const QImage image = theme.icon(which, theme.iconColor, 16).pixmap(16, 16).toImage();
        if (image.isNull())
            return 0.0;

        int longest = 0;
        for (int y = 0; y < image.height(); ++y) {
            int current = 0;
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() > 160) {
                    longest = qMax(longest, ++current);
                } else {
                    current = 0;
                }
            }
        }
        return image.width() > 0 ? double(longest) / image.width() : 0.0;
    };

    const double back = widestRun(NavIcon::Back);
    const double forward = widestRun(NavIcon::Forward);

    // Measured: a stroked chevron gives about 0.09 of the width, a filled
    // triangle about 0.37. The threshold sits between them, so this fails if the
    // shapes are ever changed back to outlines.
    QVERIFY2(back > 0.22,
             qPrintable(QStringLiteral("back is not filled (widest run %1)").arg(back)));
    QVERIFY2(forward > 0.22,
             qPrintable(QStringLiteral("forward is not filled (widest run %1)").arg(forward)));

    // The two have to point opposite ways, and the way to tell is where their
    // weight sits: each triangle is heavy at its base and light at its point, so
    // back leans right and forward leans left. Comparing the images pixel for
    // pixel would not work - rasterising a mirrored path is not bit-identical to
    // rasterising the mirror of its geometry.
    const auto inkCentroid = [&](NavIcon which, int *inkCount) {
        const QImage image = theme.icon(which, theme.iconColor, 16).pixmap(16, 16).toImage();
        double sum = 0;
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).alpha() > 160) {
                    sum += x;
                    ++count;
                }
            }
        }
        *inkCount = count;
        return count > 0 ? sum / count / qMax(1, image.width() - 1) : 0.0;
    };

    int backInk = 0;
    int forwardInk = 0;
    const double backCentre = inkCentroid(NavIcon::Back, &backInk);
    const double forwardCentre = inkCentroid(NavIcon::Forward, &forwardInk);

    QVERIFY2(backCentre > 0.52,
             qPrintable(QStringLiteral("back must lean toward its base: %1").arg(backCentre)));
    QVERIFY2(forwardCentre < 0.48, qPrintable(QStringLiteral("forward must lean toward its "
                                                            "base: %1")
                                                  .arg(forwardCentre)));

    // And they are the same size, so one does not look heavier than the other.
    const double meanInk = (backInk + forwardInk) / 2.0;
    QVERIFY2(qAbs(backInk - forwardInk) < meanInk * 0.1,
             qPrintable(QStringLiteral("the two triangles differ in size: %1 vs %2")
                            .arg(backInk)
                            .arg(forwardInk)));
}

void ThemeTest::noIconIsDrawnOversizedOrClipped()
{
    // The icons are authored on a 20x20 grid and scaled into the pixmap. That
    // scale is easy to get wrong: a QPainter on a pixmap with a device pixel
    // ratio already works in logical coordinates, so scaling by the pixel count
    // as well draws every glyph at twice its size, clipped to one corner. It
    // looked almost plausible on a Retina display, which is what made it worth a
    // test: the shapes were recognisable, just too big and cut off.
    const Theme theme = Theme::light();

    const QList<NavIcon> icons = {
        NavIcon::Back, NavIcon::Forward, NavIcon::Reload,  NavIcon::Stop,
        NavIcon::Home, NavIcon::NewTab,  NavIcon::Star,    NavIcon::History,
        NavIcon::Lock, NavIcon::Globe,   NavIcon::Close,   NavIcon::Info,
    };

    for (int size : {16, 24}) {
        for (NavIcon which : icons) {
            const QImage image
                = theme.icon(which, theme.iconColor, size).pixmap(size, size).toImage();
            QVERIFY(!image.isNull());

            // The glyph has to have a margin on every side. Ink in the outermost
            // row or column means the drawing ran off the edge, which is what
            // clipping looks like.
            int minX = image.width();
            int maxX = -1;
            int minY = image.height();
            int maxY = -1;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixelColor(x, y).alpha() > 160) {
                        minX = qMin(minX, x);
                        maxX = qMax(maxX, x);
                        minY = qMin(minY, y);
                        maxY = qMax(maxY, y);
                    }
                }
            }

            QVERIFY2(maxX >= 0, "an icon painted nothing");
            QVERIFY2(minX >= 1 && minY >= 1,
                     qPrintable(QStringLiteral("icon %1 at %2px runs off the top left")
                                    .arg(int(which))
                                    .arg(size)));
            QVERIFY2(maxX <= image.width() - 2 && maxY <= image.height() - 2,
                     qPrintable(QStringLiteral("icon %1 at %2px runs off the bottom right "
                                               "(%3,%4 of %5)")
                                    .arg(int(which))
                                    .arg(size)
                                    .arg(maxX)
                                    .arg(maxY)
                                    .arg(image.width())));

            // And it is roughly centred, which a shape drawn at the wrong scale
            // would not be.
            const double centreX = (minX + maxX) / 2.0 / image.width();
            const double centreY = (minY + maxY) / 2.0 / image.height();
            QVERIFY2(qAbs(centreX - 0.5) < 0.12,
                     qPrintable(QStringLiteral("icon %1 at %2px is off centre in x: %3")
                                    .arg(int(which))
                                    .arg(size)
                                    .arg(centreX)));
            QVERIFY2(qAbs(centreY - 0.5) < 0.12,
                     qPrintable(QStringLiteral("icon %1 at %2px is off centre in y: %3")
                                    .arg(int(which))
                                    .arg(size)
                                    .arg(centreY)));
        }
    }
}

void ThemeTest::writeIconSheet()
{
    const QString target = qEnvironmentVariable("OQB_ICON_PREVIEW");
    if (target.isEmpty())
        QSKIP("set OQB_ICON_PREVIEW=/path/to/sheet.png to write an icon sheet");

    const QList<NavIcon> icons = {
        NavIcon::Back, NavIcon::Forward,    NavIcon::Reload, NavIcon::Stop,
        NavIcon::Home, NavIcon::NewTab,     NavIcon::Star,   NavIcon::StarFilled,
        NavIcon::Menu, NavIcon::History,    NavIcon::Bookmarks, NavIcon::Info,
        NavIcon::Lock, NavIcon::Globe,      NavIcon::Close,
    };

    // Two rows: light theme then dark, at Dock-like size and magnified, so both
    // the small rendering and the geometry are visible.
    const int cell = 96;
    const int small = 18;
    const Theme lightTheme = Theme::light();
    const Theme darkTheme = Theme::dark();

    QImage sheet(cell * icons.size(), cell * 2 + 40, QImage::Format_ARGB32);
    sheet.fill(QColor(0xF3, 0xF5, 0xF8));
    {
        QPainter painter(&sheet);
        painter.fillRect(0, cell + 20, sheet.width(), cell, QColor(0x1E, 0x21, 0x26));

        for (int i = 0; i < icons.size(); ++i) {
            const int x = i * cell;
            const QIcon light = lightTheme.icon(icons.at(i), lightTheme.iconColor, small);
            const QIcon dark = darkTheme.icon(icons.at(i), darkTheme.iconColor, small);
            painter.drawPixmap(x + (cell - small) / 2, (20 - small) / 2 + 0, light.pixmap(small, small));
            painter.drawPixmap(x + (cell - small) / 2, cell + 20 + (20 - small) / 2, dark.pixmap(small, small));

            // Magnified, so the geometry can be judged.
            const QImage big = lightTheme.icon(icons.at(i), lightTheme.iconColor, 72).pixmap(72, 72).toImage();
            painter.drawImage(x + (cell - 72) / 2, 30, big);
        }
        painter.end();
    }

    QVERIFY2(sheet.save(target), qPrintable(QStringLiteral("cannot write ") + target));
}

QTEST_MAIN(ThemeTest)
#include "tst_theme.moc"
