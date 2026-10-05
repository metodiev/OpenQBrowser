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
    void paintBackground(QPainter *painter, Box *box);
    void paintBorders(QPainter *painter, Box *box);
    void paintText(QPainter *painter, Box *box);
    void paintBullet(QPainter *painter, Box *box);
    void paintBoxModelOverlay(QPainter *painter, Box *box);

    /// Position of a box relative to the viewport after scrolling.
    QRectF viewportRect(const Box *box) const;

    Options m_options;

    /// The colour a text run inherits, tracked during the walk so that a nested
    /// element with no colour of its own still draws correctly.
    QColor m_currentTextColor;
};

} // namespace oqb::renderer
