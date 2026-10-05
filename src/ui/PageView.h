#pragma once

#include <QPoint>
#include <QWidget>

#include "network/Url.h"

namespace oqb::browser {
class Tab;
}
namespace oqb::dom {
class Element;
}

namespace oqb::ui {

/// Draws a tab's page and turns mouse and keyboard input into navigation.
///
/// The view owns no state of its own beyond the scroll position and the
/// selection: it asks the tab for the box tree and the layout, and paints
/// whatever it finds. That keeps the rendering path identical to the headless
/// one, which is why a screenshot from the command line matches the window.
class PageView : public QWidget
{
    Q_OBJECT

public:
    explicit PageView(QWidget *parent = nullptr);
    ~PageView() override;

    /// Points the view at a tab. The view repaints whenever the tab changes.
    void setTab(browser::Tab *tab);
    browser::Tab *tab() const { return m_tab; }

    /// Repaints from the tab's current document.
    void refresh();

    /// Scrolls by a number of pixels, clamped to the page.
    void scrollBy(int dx, int dy);
    void scrollTo(int x, int y);
    QPoint scrollPosition() const { return m_scroll; }

    /// The size of the laid-out document, which is what the scroll bars cover.
    QSize documentSize() const;

    /// Shows or hides the box-model overlay, the first DevTools feature.
    void setShowBoxModel(bool show);
    bool showsBoxModel() const { return m_showBoxModel; }

    /// The link under a point, or an empty URL when there is none. This is what
    /// makes a click navigate and the status bar show a destination.
    network::Url linkAt(const QPoint &position) const;

signals:
    /// The user asked to follow a link. The window decides how to open it.
    void linkActivated(const oqb::network::Url &url, bool newTab);
    /// The document metrics changed, so the scroll bars need updating.
    void documentSizeChanged(const QSize &size);
    /// The hovered link changed, for the status bar.
    void linkHovered(const QString &url);
    /// A page asked to be reloaded.
    void reloadRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    /// The link element under a viewport point, walking the box tree.
    const dom::Element *linkElementAt(const QPoint &position) const;

    browser::Tab *m_tab = nullptr;
    QPoint m_scroll;
    bool m_showBoxModel = false;
    /// True once a press has happened, so a release on the same link counts as
    /// a click rather than a drag.
    bool m_pressed = false;
    QPoint m_pressPosition;
};

} // namespace oqb::ui
