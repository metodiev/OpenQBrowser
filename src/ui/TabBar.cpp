#include "ui/TabBar.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QToolButton>

namespace oqb::ui {

TabBar::TabBar(QWidget *parent)
    : QTabBar(parent)
{
    setExpanding(false);
    setMovable(true);
    setTabsClosable(false); // The close button is drawn here instead.
    setUsesScrollButtons(true);
    setElideMode(Qt::ElideRight);
    setDrawBase(false);
    setFocusPolicy(Qt::NoFocus);
}

void TabBar::tabInserted(int index)
{
    QTabBar::tabInserted(index);
    installCloseButton(index);
}

void TabBar::tabRemoved(int index)
{
    // Qt deletes the tab's button with the tab, so there is nothing to release
    // here; the override exists to keep the pair visible in one place.
    QTabBar::tabRemoved(index);
}

void TabBar::setTheme(const Theme &theme)
{
    m_theme = &theme;
    for (int i = 0; i < count(); ++i) {
        if (auto *button = qobject_cast<QToolButton *>(tabButton(i, QTabBar::RightSide)))
            button->setIcon(m_theme->icon(NavIcon::Close, m_theme->iconColor, 11));
    }
}

void TabBar::installCloseButton(int index)
{
    if (!m_theme)
        return;

    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("TabCloseButton"));
    button->setIcon(m_theme->icon(NavIcon::Close, m_theme->iconColor, 11));
    button->setIconSize(QSize(11, 11));
    button->setFixedSize(18, 18);
    button->setCursor(Qt::ArrowCursor);
    button->setToolTip(tr("Close tab"));
    button->setFocusPolicy(Qt::NoFocus);

    connect(button, &QToolButton::clicked, this, [this, button] {
        const int tabIndex = indexOfCloseButton(button);
        if (tabIndex >= 0)
            emit closeRequested(tabIndex);
    });

    setTabButton(index, QTabBar::RightSide, button);
}

int TabBar::indexOfCloseButton(QWidget *button) const
{
    for (int i = 0; i < count(); ++i) {
        if (tabButton(i, QTabBar::RightSide) == button)
            return i;
    }
    return -1;
}

void TabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A double click on empty space is the gesture every browser accepts as
    // "give me a new tab". A double click on a tab keeps whatever Qt does with
    // it, which is nothing by default.
    if (event->button() == Qt::LeftButton && tabAt(event->pos()) < 0) {
        emit newTabRequested();
        return;
    }

    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::mousePressEvent(QMouseEvent *event)
{
    // A middle click closes the tab under the pointer, which is what makes a
    // background tab cheap to dismiss.
    if (event->button() == Qt::MiddleButton) {
        const int index = tabAt(event->pos());
        if (index >= 0) {
            emit closeRequested(index);
            return;
        }
    }

    QTabBar::mousePressEvent(event);
}

} // namespace oqb::ui
