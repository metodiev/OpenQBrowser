#pragma once

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QString>

#include "renderer/BoxTree.h"

namespace oqb::renderer {

/// Draws a laid-out box tree onto a QPainter.
///
/// The painter is deliberately stateless apart from its options: it walks the
/// tree it is given and draws each box in turn, which makes it usable for the
/// on-screen view, the headless screenshot mode and the future print path
/// without change.
class Painter
{
public:
    struct Options
    {
        /// The document background, used when the page sets none.
        QColor defaultBackground = Qt::white;
        /// The default text colour, used when no style provides one.
        QColor defaultTextColor = Qt::black;
        /// Scroll offset: the document is translated by the negative of this.
        double scrollX = 0;
        double scrollY = 0;
        /// The visible area, needed only by sticky positioning, which is the one
        /// feature whose result depends on where the viewport is rather than on
        /// the document alone. Zero means unknown, in which case nothing sticks.
        double viewportWidth = 0;
        double viewportHeight = 0;
        /// When true, box outlines and margins are drawn over the page. This is
        /// the beginning of a DevTools feature and is off by default.
        bool showBoxModel = false;
        /// Selection highlight, in document coordinates. Empty for none.
        QRectF selection;
        QColor selectionColor = QColor(0, 120, 215, 90);
    };

    Painter() = default;
    explicit Painter(const Options &options);

    /// Paints `root` onto `painter`. The caller has already set up the clip and
    /// the scroll translation.
    void paint(QPainter *painter, Box *root);

    /// Renders `root` to a new image of the given size, with default options.
    /// Used by the screenshot mode and by the tests.
    static QImage renderToImage(Box *root, int width, int height);

    /// Renders with explicit options.
    static QImage renderToImage(Box *root, int width, int height, const Options &options);

    /// The colour to fill the canvas with, taking the document background from
    /// the root box's style when one is set.
    QColor canvasColor(Box *root) const;

private:
    void paintBox(QPainter *painter, Box *box);

    /// The viewport rectangle a sticky box is constrained to, in document
    /// coordinates, or an empty rect when the box does not stick. Sticky
    /// positioning depends on the scroll position, which changes without a
    /// re-layout, so it is resolved while painting rather than baked into the
    /// box's geometry.
    QRectF stickyViewportFor(const Box *box) const;

    /// Draws a sticky box's subtree with the box's own geometry adjusted for the
    /// scroll position.
    void paintStickyBox(QPainter *painter, Box *box);

    /// Draws a box and its subtree without the sticky adjustment. This is what
    /// paintStickyBox calls once it has moved the box, so that the sticky branch
    /// is not taken a second time - which would recurse forever.
    void paintBoxContents(QPainter *painter, Box *box);

    /// Paints a box's children in stacking order rather than document order.
    ///
    /// CSS 2.2 paints in phases: the in-flow content first, then the positioned
    /// boxes with a positive z-index on top of it. Without this an absolutely
    /// positioned dropdown or modal - which is always written after the content
    /// it covers in the document anyway - would be drawn underneath a later
    /// sibling, which is exactly what it exists to cover.
    void paintChildrenInStackingOrder(QPainter *painter, Box *box);
    void paintBackground(QPainter *painter, Box *box);
    void paintBorders(QPainter *painter, Box *box);
    void paintText(QPainter *painter, Box *box);
    void paintBullet(QPainter *painter, Box *box);
    void paintBoxModelOverlay(QPainter *painter, Box *box);

    /// True while a sticky box's subtree is being drawn, so a nested sticky box
    /// is not adjusted against the viewport a second time.
    bool m_inStickyPaint = false;

    /// Position of a box relative to the viewport after scrolling.
    QRectF viewportRect(const Box *box) const;

    Options m_options;

    /// The colour a text run inherits, tracked during the walk so that a nested
    /// element with no colour of its own still draws correctly.
    QColor m_currentTextColor;
};

} // namespace oqb::renderer
