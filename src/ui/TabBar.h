#pragma once

#include <QTabBar>

class QToolButton;

namespace oqb::ui {

struct Theme;

/// The strip of tabs above the toolbar.
///
/// It exists for the interactions a plain QTabBar does not have and a browser
/// needs: a visible way to open a tab, a close button that matches the theme,
/// and the two shortcuts users reach for without reading a menu - double-click
/// on empty space, and a middle click to close.
class TabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit TabBar(QWidget *parent = nullptr);

    /// Re-paints the close buttons for a new theme, and remembers it so buttons
    /// created for later tabs match.
    void setTheme(const Theme &theme);
    const Theme &theme() const { return *m_theme; }

signals:
    /// A close button was pressed. The window removes the tab, because only it
    /// knows what else the tab owns.
    void closeRequested(int index);
    /// The user asked for a new tab by double-clicking the empty part of the
    /// strip, which is a gesture every browser accepts.
    void newTabRequested();

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void tabInserted(int index) override;
    void tabRemoved(int index) override;

private:
    /// Gives `index` a close button drawn in the theme's colours.
    void installCloseButton(int index);
    /// The index whose close button is `button`, or -1. Buttons are found this
    /// way rather than remembered by index, because indices move when a tab is
    /// dragged or closed.
    int indexOfCloseButton(QWidget *button) const;

    const Theme *m_theme = nullptr;
};

} // namespace oqb::ui
