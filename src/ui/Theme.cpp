#include "ui/Theme.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyleHints>

#include <cmath>

namespace oqb::ui {

namespace {

/// Draws one icon into a unit square and scales it to the requested size, so
/// the geometry below can be written once against a 20x20 grid.
void paintIcon(QPainter *painter, NavIcon which, const QColor &color)
{
    // Every icon is authored on a 20x20 grid; the painter is already scaled so
    // that this box fills the target pixmap.
    QPen pen(color, 1.9);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    // The navigation controls use the same weight as the rest of the chrome.
    // Drawn heavier than this they read as chunky rather than as part of a
    // finished interface: at a 16px icon a 2.3-unit stroke is 11.5% of the box,
    // where a browser's own glyphs sit near 9%.
    QPen navPen(color, 1.75);
    navPen.setCapStyle(Qt::RoundCap);
    navPen.setJoinStyle(Qt::RoundJoin);

    const QPen hairline = [&] {
        QPen p(color, 1.6);
        p.setCapStyle(Qt::RoundCap);
        p.setJoinStyle(Qt::RoundJoin);
        return p;
    }();

    painter->setRenderHint(QPainter::Antialiasing, true);

    switch (which) {
    case NavIcon::Back:
    case NavIcon::Forward: {
        // A solid triangle rather than a stroke, which is a shape that survives
        // being drawn at 16 pixels: a thin chevron loses its shape to
        // antialiasing, where a filled area keeps its silhouette.
        //
        // The triangle is filled *and* stroked with a round-joined pen of the
        // same colour. The stroke is what rounds the corners - a raw filled
        // triangle looks like a shard at this size, because its three points are
        // the sharpest thing on the toolbar.
        const bool back = which == NavIcon::Back;
        const double tipX = back ? 6.0 : 14.0;
        const double baseX = back ? 13.5 : 6.5;

        QPainterPath triangle;
        triangle.moveTo(tipX, 10.0);
        triangle.lineTo(baseX, 4.8);
        triangle.lineTo(baseX, 15.2);
        triangle.closeSubpath();

        QPen solid(color, 1.7);
        solid.setJoinStyle(Qt::RoundJoin);
        solid.setCapStyle(Qt::RoundCap);
        painter->setPen(solid);
        painter->setBrush(color);
        painter->drawPath(triangle);
        break;
    }
    case NavIcon::Reload: {
        // A circle broken at the top right, with the arrow head at the top
        // pointing the way the stroke travels. The head is filled and its base
        // sits on the end of the arc, so the two read as one mark; an outline
        // head placed beside the gap looked like a separate floating triangle.
        const QRectF circle(4.5, 4.5, 11.0, 11.0);
        painter->setPen(navPen);
        painter->setBrush(Qt::NoBrush);
        // The sweep starts at the lower edge of the gap and runs clockwise,
        // ending at the top of the circle.
        painter->drawArc(circle, 48 * 16, -315 * 16);

        const QPointF tip(13.6, 4.5);
        QPainterPath head;
        head.moveTo(tip);
        head.lineTo(10.1, 2.4);
        head.lineTo(10.1, 6.8);
        head.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawPath(head);
        break;
    }
    case NavIcon::Stop: {
        painter->setPen(navPen);
        painter->drawLine(QPointF(5.9, 5.9), QPointF(14.1, 14.1));
        painter->drawLine(QPointF(14.1, 5.9), QPointF(5.9, 14.1));
        break;
    }
    case NavIcon::Home: {
        // A house, rather than a roof floating over a box: the eaves line is
        // where the roof and the walls meet, so the two strokes join visually.
        QPainterPath roof;
        roof.moveTo(3.3, 9.9);
        roof.lineTo(10.0, 3.5);
        roof.lineTo(16.7, 9.9);

        QPainterPath walls;
        walls.moveTo(5.7, 9.5);
        walls.lineTo(5.7, 16.3);
        walls.lineTo(14.3, 16.3);
        walls.lineTo(14.3, 9.5);

        painter->setPen(navPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(roof);
        painter->drawPath(walls);

        // The door is what makes the outline read as a house at tab size rather
        // than as an empty box under a triangle.
        QPainterPath door;
        door.moveTo(8.4, 16.3);
        door.lineTo(8.4, 12.5);
        door.lineTo(11.6, 12.5);
        door.lineTo(11.6, 16.3);
        painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPath(door);
        break;
    }
    case NavIcon::NewTab: {
        painter->setPen(pen);
        painter->drawLine(QPointF(10.0, 4.6), QPointF(10.0, 15.4));
        painter->drawLine(QPointF(4.6, 10.0), QPointF(15.4, 10.0));
        break;
    }
    case NavIcon::Star:
    case NavIcon::StarFilled: {
        QPainterPath star;
        constexpr double cx = 10.0;
        constexpr double cy = 10.4;
        constexpr double outer = 6.4;
        constexpr double inner = 2.9;
        for (int i = 0; i < 10; ++i) {
            const double radius = (i % 2 == 0) ? outer : inner;
            // Start at the top point so the star sits upright.
            const double angle = -M_PI / 2.0 + i * M_PI / 5.0;
            const QPointF point(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
            if (i == 0)
                star.moveTo(point);
            else
                star.lineTo(point);
        }
        star.closeSubpath();

        if (which == NavIcon::StarFilled) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
        } else {
            painter->setPen(hairline);
            painter->setBrush(Qt::NoBrush);
        }
        painter->drawPath(star);
        break;
    }
    case NavIcon::Menu: {
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        for (int i = 0; i < 3; ++i)
            painter->drawEllipse(QPointF(10.0, 5.6 + i * 4.4), 1.5, 1.5);
        break;
    }
    case NavIcon::History: {
        painter->setPen(hairline);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QRectF(4.2, 4.2, 11.6, 11.6));

        QPainterPath hands;
        hands.moveTo(10.0, 6.6);
        hands.lineTo(10.0, 10.3);
        hands.lineTo(13.0, 11.9);
        painter->setPen(pen);
        painter->drawPath(hands);
        break;
    }
    case NavIcon::Bookmarks: {
        QPainterPath mark;
        mark.moveTo(6.0, 3.6);
        mark.lineTo(14.0, 3.6);
        mark.lineTo(14.0, 16.4);
        mark.lineTo(10.0, 13.2);
        mark.lineTo(6.0, 16.4);
        mark.closeSubpath();
        painter->setPen(hairline);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(mark);
        break;
    }
    case NavIcon::Info: {
        painter->setPen(hairline);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QRectF(4.2, 4.2, 11.6, 11.6));

        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(QPointF(10.0, 7.2), 1.05, 1.05);
        painter->setPen(pen);
        painter->drawLine(QPointF(10.0, 9.6), QPointF(10.0, 13.4));
        break;
    }
    case NavIcon::Lock: {
        // Body first, then the shackle above it.
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawRoundedRect(QRectF(5.4, 9.2, 9.2, 7.0), 1.8, 1.8);

        QPainterPath shackle;
        shackle.moveTo(7.4, 9.2);
        shackle.lineTo(7.4, 7.4);
        shackle.arcTo(QRectF(7.4, 4.4, 5.2, 5.2), 180, -180);
        shackle.lineTo(12.6, 9.2);
        painter->setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(shackle);
        break;
    }
    case NavIcon::Globe: {
        painter->setPen(hairline);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QRectF(4.2, 4.2, 11.6, 11.6));
        painter->drawArc(QRectF(7.2, 4.2, 5.6, 11.6), 90 * 16, 180 * 16);
        painter->drawArc(QRectF(7.2, 4.2, 5.6, 11.6), 270 * 16, 180 * 16);
        painter->drawLine(QPointF(4.6, 10.0), QPointF(15.4, 10.0));
        break;
    }
    case NavIcon::Close: {
        painter->setPen(pen);
        painter->drawLine(QPointF(6.4, 6.4), QPointF(13.6, 13.6));
        painter->drawLine(QPointF(13.6, 6.4), QPointF(6.4, 13.6));
        break;
    }
    }
}

/// Paints `which` into a pixmap at the device's pixel ratio, so the strokes land
/// on whole pixels and the icon stays sharp on a Retina display.
QPixmap paintPixmap(NavIcon which, const QColor &color, int size, qreal dpr)
{
    const int pixelSize = qMax(1, static_cast<int>(std::ceil(size * dpr)));
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    // The icon is authored on a 20x20 grid. The scale is by the *logical* size,
    // not by the pixel count: a QPainter drawing onto a pixmap whose
    // devicePixelRatio is set already works in logical coordinates, so scaling
    // by the pixel count as well drew everything at twice its size and clipped
    // it to the top-left corner on a Retina display.
    painter.scale(size / 20.0, size / 20.0);
    paintIcon(&painter, which, color);
    painter.end();

    return pixmap;
}

} // namespace

Theme Theme::light()
{
    Theme theme;
    theme.isDark = false;

    theme.windowBackground = QColor(0xFF, 0xFF, 0xFF);
    theme.chromeBackground = QColor(0xF3, 0xF5, 0xF8);
    theme.chromeBorder = QColor(0xDC, 0xE0, 0xE6);
    theme.tabText = QColor(0x53, 0x5A, 0x66);
    theme.tabTextActive = QColor(0x1B, 0x21, 0x2B);
    theme.tabHover = QColor(0xE6, 0xEA, 0xF0);
    theme.tabActive = QColor(0xFF, 0xFF, 0xFF);
    theme.tabActiveBorder = QColor(0xDC, 0xE0, 0xE6);
    theme.iconColor = QColor(0x45, 0x4C, 0x57);
    theme.iconColorActive = QColor(0x1B, 0x21, 0x2B);
    theme.iconColorDisabled = QColor(0xB0, 0xB6, 0xBF);
    theme.fieldBackground = QColor(0xFF, 0xFF, 0xFF);
    theme.fieldBorder = QColor(0xD6, 0xDB, 0xE2);
    theme.fieldText = QColor(0x1B, 0x21, 0x2B);
    theme.fieldPlaceholder = QColor(0x8A, 0x92, 0x9E);
    theme.accent = QColor(0x1D, 0x74, 0xD8);
    theme.statusText = QColor(0x5F, 0x67, 0x72);

    return theme;
}

Theme Theme::dark()
{
    Theme theme;
    theme.isDark = true;

    theme.windowBackground = QColor(0x1E, 0x21, 0x26);
    theme.chromeBackground = QColor(0x27, 0x2B, 0x31);
    theme.chromeBorder = QColor(0x35, 0x3A, 0x42);
    theme.tabText = QColor(0xA8, 0xB0, 0xBC);
    theme.tabTextActive = QColor(0xF2, 0xF5, 0xF9);
    theme.tabHover = QColor(0x33, 0x38, 0x40);
    theme.tabActive = QColor(0x1E, 0x21, 0x26);
    theme.tabActiveBorder = QColor(0x35, 0x3A, 0x42);
    theme.iconColor = QColor(0xC2, 0xC9, 0xD4);
    theme.iconColorActive = QColor(0xF2, 0xF5, 0xF9);
    theme.iconColorDisabled = QColor(0x5E, 0x66, 0x70);
    theme.fieldBackground = QColor(0x1A, 0x1D, 0x22);
    theme.fieldBorder = QColor(0x3A, 0x40, 0x49);
    theme.fieldText = QColor(0xF2, 0xF5, 0xF9);
    theme.fieldPlaceholder = QColor(0x86, 0x8F, 0x9C);
    theme.accent = QColor(0x5A, 0xA9, 0xFF);
    theme.statusText = QColor(0x9A, 0xA3, 0xAF);

    return theme;
}

Theme Theme::forCurrentColorScheme()
{
    // Qt reports what the desktop is set to, so a user with a dark macOS
    // appearance gets a dark window without configuring anything.
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark)
        return dark();
    return light();
}

QString Theme::styleSheet() const
{
    const auto rgba = [](const QColor &c) { return c.name(QColor::HexRgb); };

    return QStringLiteral(R"(
/* ---------------------------------------------------------------- window */
QMainWindow, QWidget#BrowserChrome, QWidget#TabRow, QWidget#ToolRow {
    background: %(chrome)s;
}
QMainWindow::separator { height: 0px; width: 0px; }

/* ---------------------------------------------------------------- tabs */
QTabBar {
    background: transparent;
    qproperty-drawBase: 0;
}
QTabBar::tab {
    background: transparent;
    color: %(tabText)s;
    border: 1px solid transparent;
    border-radius: 8px;
    padding: 6px 10px;
    margin: 0px 2px;
    min-width: 108px;
    max-width: 220px;
    font-size: 12px;
}
QTabBar::tab:hover {
    background: %(tabHover)s;
}
QTabBar::tab:selected {
    background: %(tabActive)s;
    color: %(tabTextActive)s;
    border: 1px solid %(tabActiveBorder)s;
}

/* ------------------------------------------------------------ buttons */
QToolButton {
    background: transparent;
    border: none;
    border-radius: 8px;
    padding: 5px;
}
QToolButton:hover {
    background: %(tabHover)s;
}
QToolButton:pressed {
    background: %(chromeBorder)s;
}
/* A disabled control must not light up under the pointer; without this rule the
   hover colour above still wins, because both selectors weigh the same and the
   later one decides. */
QToolButton:disabled {
    background: transparent;
}
QToolButton#NewTabButton {
    padding: 5px;
}
QToolButton#TabCloseButton {
    border-radius: 6px;
    padding: 2px;
}
QToolButton#TabCloseButton:hover {
    background: %(closeHover)s;
}

/* --------------------------------------------------------- address bar */
QLineEdit#AddressBar {
    background: %(field)s;
    color: %(fieldText)s;
    border: 1px solid %(fieldBorder)s;
    border-radius: 15px;
    padding: 6px 12px;
    selection-background-color: %(accent)s;
    selection-color: #FFFFFF;
    font-size: 13px;
}
QLineEdit#AddressBar:focus {
    background: %(field)s;
    border: 1px solid %(accent)s;
}
QLineEdit#AddressBar::placeholder {
    color: %(placeholder)s;
}

/* ------------------------------------------------------------ progress */
QProgressBar#LoadProgress {
    background: transparent;
    border: none;
    height: 3px;
}
QProgressBar#LoadProgress::chunk {
    background: %(accent)s;
    border-radius: 1px;
}

/* ---------------------------------------------------------- status bar */
QStatusBar {
    background: %(chrome)s;
    color: %(statusText)s;
    border-top: 1px solid %(chromeBorder)s;
    font-size: 12px;
}
QStatusBar::item { border: none; }

/* --------------------------------------------------------------- menus */
QMenuBar {
    background: %(chrome)s;
    color: %(fieldText)s;
}
QMenuBar::item:selected { background: %(tabHover)s; }
QMenu {
    background: %(menuBackground)s;
    color: %(fieldText)s;
    border: 1px solid %(chromeBorder)s;
    padding: 6px;
}
QMenu::item {
    padding: 6px 26px 6px 20px;
    border-radius: 6px;
}
QMenu::item:selected { background: %(accent)s; color: #FFFFFF; }
QMenu::separator { height: 1px; background: %(chromeBorder)s; margin: 6px 8px; }

/* --------------------------------------------------------- dock widget */
QDockWidget {
    color: %(fieldText)s;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
}
QDockWidget::title {
    background: %(chrome)s;
    padding: 5px 8px;
}
)")
        .replace(QStringLiteral("%(chrome)s"), rgba(chromeBackground))
        .replace(QStringLiteral("%(chromeBorder)s"), rgba(chromeBorder))
        .replace(QStringLiteral("%(tabText)s"), rgba(tabText))
        .replace(QStringLiteral("%(tabTextActive)s"), rgba(tabTextActive))
        .replace(QStringLiteral("%(tabHover)s"), rgba(tabHover))
        .replace(QStringLiteral("%(tabActive)s"), rgba(tabActive))
        .replace(QStringLiteral("%(tabActiveBorder)s"), rgba(tabActiveBorder))
        .replace(QStringLiteral("%(field)s"), rgba(fieldBackground))
        .replace(QStringLiteral("%(fieldBorder)s"), rgba(fieldBorder))
        .replace(QStringLiteral("%(fieldText)s"), rgba(fieldText))
        .replace(QStringLiteral("%(placeholder)s"), rgba(fieldPlaceholder))
        .replace(QStringLiteral("%(accent)s"), rgba(accent))
        .replace(QStringLiteral("%(statusText)s"), rgba(statusText))
        .replace(QStringLiteral("%(closeHover)s"), rgba(isDark ? QColor(0x4A, 0x51, 0x5A)
                                                               : QColor(0xD3, 0xD9, 0xE0)))
        .replace(QStringLiteral("%(menuBackground)s"), rgba(windowBackground));
}

QIcon Theme::icon(NavIcon which, const QColor &color, int size) const
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    return QIcon(paintPixmap(which, color, size, dpr));
}

QIcon Theme::icon(NavIcon which, const QColor &color, const QColor &disabledColor,
                  int size) const
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;

    QIcon icon;
    // Added for both modes, in that order: with a Disabled pixmap present Qt
    // uses it instead of deriving one, so the colour asked for is the colour
    // drawn.
    icon.addPixmap(paintPixmap(which, color, size, dpr), QIcon::Normal);
    icon.addPixmap(paintPixmap(which, disabledColor, size, dpr), QIcon::Disabled);
    return icon;
}

QIcon Theme::icon(NavIcon which, int size) const
{
    return icon(which, iconColor, size);
}

} // namespace oqb::ui
