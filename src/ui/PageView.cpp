#include "ui/PageView.h"

#include "dom/Document.h"

#include "browser/Tab.h"
#include "renderer/BoxTree.h"
#include "renderer/Layout.h"
#include "renderer/Painter.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

namespace oqb::ui {
namespace {

/// Walks the box tree looking for the deepest box whose border box contains the
/// point, returning the element that generated it.
const dom::Element *elementAt(const renderer::Box *box, double x, double y)
{
    if (!box)
        return nullptr;

    // Children are drawn on top, so they are searched first.
    for (const auto &child : box->children()) {
        if (const dom::Element *found = elementAt(child.get(), x, y))
            return found;
    }

    if (box->borderBox().contains(x, y)) {
        if (const dom::Element *element = box->element())
            return element;
    }
    return nullptr;
}

/// True when the element is a link: an <a> with an href.
bool isLink(const dom::Element *element)
{
    return element && element->isTag(QStringLiteral("a"))
        && element->hasAttribute(QStringLiteral("href"));
}

} // namespace

PageView::PageView(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    // The widget fills its area with the page background, so no palette colour
    // shows through during a resize.
    setAutoFillBackground(false);
}

PageView::~PageView() = default;

void PageView::setTab(browser::Tab *tab)
{
    if (m_tab == tab)
        return;

    if (m_tab)
        m_tab->disconnect(this);

    m_tab = tab;
    m_scroll = QPoint(0, 0);

    if (m_tab) {
        connect(m_tab, &browser::Tab::stateChanged, this, [this] { refresh(); });
        connect(m_tab, &browser::Tab::loadFinished, this, [this] {
            // A new document starts at the top, as every browser does.
            m_scroll = QPoint(0, 0);
            refresh();
        });
    }

    refresh();
}

QSize PageView::documentSize() const
{
    if (!m_tab || !m_tab->page()->boxTree())
        return size();

    const renderer::LayoutResult &layout = m_tab->page()->layout();
    return QSize(static_cast<int>(layout.documentWidth),
                 static_cast<int>(layout.documentHeight));
}

void PageView::refresh()
{
    const QSize document = documentSize();
    emit documentSizeChanged(document);

    // Keep the scroll position inside the document after a resize or a load.
    m_scroll.setX(qBound(0, m_scroll.x(), qMax(0, document.width() - width())));
    m_scroll.setY(qBound(0, m_scroll.y(), qMax(0, document.height() - height())));

    update();
}

void PageView::scrollBy(int dx, int dy)
{
    scrollTo(m_scroll.x() + dx, m_scroll.y() + dy);
}

void PageView::scrollTo(int x, int y)
{
    const QSize document = documentSize();
    const QPoint clamped(qBound(0, x, qMax(0, document.width() - width())),
                         qBound(0, y, qMax(0, document.height() - height())));

    if (clamped != m_scroll) {
        m_scroll = clamped;
        emit documentSizeChanged(document);
        update();
    }
}

void PageView::setShowBoxModel(bool show)
{
    if (m_showBoxModel == show)
        return;
    m_showBoxModel = show;
    update();
}

const dom::Element *PageView::linkElementAt(const QPoint &position) const
{
    if (!m_tab || !m_tab->page()->boxTree())
        return nullptr;

    const double x = position.x() + m_scroll.x();
    const double y = position.y() + m_scroll.y();

    // The hit test walks up from the deepest box so that text inside a link
    // still resolves to the link.
    const dom::Element *element = elementAt(m_tab->page()->boxTree(), x, y);
    for (const dom::Element *candidate = element; candidate;
         candidate = candidate->parentElement()) {
        if (isLink(candidate))
            return candidate;
    }
    return nullptr;
}

network::Url PageView::linkAt(const QPoint &position) const
{
    const dom::Element *link = linkElementAt(position);
    if (!link)
        return {};

    return m_tab->page()->document()->resolveUrl(link->attribute(QStringLiteral("href")));
}

void PageView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);

    if (!m_tab || !m_tab->page()->boxTree()) {
        painter.fillRect(rect(), Qt::white);
        return;
    }

    const renderer::Box *root = m_tab->page()->boxTree();

    renderer::Painter::Options options;
    options.scrollX = m_scroll.x();
    options.scrollY = m_scroll.y();
    options.showBoxModel = m_showBoxModel;

    // The canvas colour comes from the page, so a dark page does not flash
    // white while it loads.
    renderer::Painter pagePainter(options);
    painter.fillRect(rect(), pagePainter.canvasColor(const_cast<renderer::Box *>(root)));

    pagePainter.paint(&painter, const_cast<renderer::Box *>(root));
}

void PageView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refresh();
}

void PageView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        m_pressPosition = event->position().toPoint();
        setFocus(Qt::MouseFocusReason);
    }
    QWidget::mousePressEvent(event);
}

void PageView::mouseMoveEvent(QMouseEvent *event)
{
    const network::Url link = linkAt(event->position().toPoint());
    emit linkHovered(link.isValid() ? link.toString() : QString());

    // A pointing hand over a link is the affordance every browser provides.
    setCursor(link.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);

    QWidget::mouseMoveEvent(event);
}

void PageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_pressed) {
        m_pressed = false;

        const QPoint release = event->position().toPoint();
        // Only treat it as a click when the pointer barely moved, so a drag
        // (a future selection gesture) does not navigate.
        if ((release - m_pressPosition).manhattanLength() < 4) {
            const network::Url link = linkAt(release);
            if (link.isValid()) {
                const bool newTab = event->modifiers().testFlag(Qt::ControlModifier)
                    || event->modifiers().testFlag(Qt::MetaModifier);
                emit linkActivated(link, newTab);
            }
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void PageView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        scrollBy(0, height() * 4 / 5);
        return;
    case Qt::Key_Down:
        scrollBy(0, 40);
        return;
    case Qt::Key_Up:
        scrollBy(0, -40);
        return;
    case Qt::Key_PageDown:
        scrollBy(0, height());
        return;
    case Qt::Key_PageUp:
        scrollBy(0, -height());
        return;
    case Qt::Key_Home:
        scrollTo(0, 0);
        return;
    case Qt::Key_End:
        scrollTo(0, documentSize().height());
        return;
    case Qt::Key_Left:
        scrollBy(-40, 0);
        return;
    case Qt::Key_Right:
        scrollBy(40, 0);
        return;
    case Qt::Key_F5:
        emit reloadRequested();
        return;
    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void PageView::wheelEvent(QWheelEvent *event)
{
    // Qt reports wheel movement in eighths of a degree; 120 units is one notch,
    // which scrolls a fixed distance so that scrolling feels the same on every
    // input device.
    const QPoint delta = event->angleDelta();
    scrollBy(-delta.x() / 3, -delta.y() / 3);
    event->accept();
}

} // namespace oqb::ui
