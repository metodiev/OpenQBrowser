#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace oqb::ui {

/// The icons the window's own controls use.
///
/// They are painted rather than loaded from files so that a single definition
/// serves both themes and every display scale: a vector drawn at the requested
/// size stays crisp on a Retina screen, and a theme switch is a colour change
/// rather than a second set of assets.
enum class NavIcon {
    Back,
    Forward,
    Reload,
    Stop,
    Home,
    NewTab,
    Star,
    StarFilled,
    Menu,
    History,
    Bookmarks,
    Info,
    Lock,
    Globe,
    Close,
};

/// The window's palette and stylesheet.
///
/// One structure holds every colour the chrome uses, so the light and dark
/// variants cannot drift apart: adding a colour to one means adding it to the
/// other, and the stylesheet reads from the structure rather than repeating
/// hex literals.
struct Theme
{
    bool isDark = false;

    QColor windowBackground;
    QColor chromeBackground;   ///< The tab strip and toolbar surface.
    QColor chromeBorder;       ///< The hairline under the chrome.
    QColor tabText;
    QColor tabTextActive;
    QColor tabHover;
    QColor tabActive;
    QColor tabActiveBorder;
    QColor iconColor;
    QColor iconColorActive;
    /// The colour a control that cannot be used is drawn in. It is an explicit
    /// colour rather than a faded one: a faded icon composites unpredictably
    /// over the toolbar, and the two themes need different amounts of contrast
    /// against very different backgrounds.
    QColor iconColorDisabled;
    QColor fieldBackground;
    QColor fieldBorder;
    QColor fieldText;
    QColor fieldPlaceholder;
    QColor accent;
    QColor statusText;

    /// The theme the rest of the system is using, so the window follows the
    /// user's appearance setting rather than fighting it.
    static Theme forCurrentColorScheme();
    static Theme light();
    static Theme dark();

    /// The Qt style sheet for the whole window, built from this theme.
    QString styleSheet() const;

    /// A crisp icon in `color`, painted at `size` logical pixels.
    QIcon icon(NavIcon which, const QColor &color, int size = 18) const;

    /// An icon that also knows how it looks when its control is disabled.
    ///
    /// The disabled rendering is supplied as a pixmap in QIcon::Disabled mode,
    /// which stops Qt from generating its own version by fading the normal one.
    /// That matters because Qt's fade is applied on top of whatever colour is
    /// given: a colour chosen to read as "unavailable" came out almost invisible
    /// once Qt had faded it again.
    QIcon icon(NavIcon which, const QColor &color, const QColor &disabledColor,
               int size = 18) const;

    /// The same icon in the theme's default icon colour.
    QIcon icon(NavIcon which, int size = 18) const;
};

} // namespace oqb::ui
