#include "renderer/Painter.h"

#include "renderer/Layout.h"

#include <QFontMetricsF>
#include <QPainterPath>

namespace oqb::renderer {
namespace {

/// Interprets a CSS length such as "10px 20px" for border-radius. Only a single
/// radius is honoured; elliptical corners are reported by the inspector.
double parseRadius(const QString &text, double fallback)
{
    QString value = text.trimmed();
    const int space = value.indexOf(u' ');
    if (space > 0)
        value = value.left(space);
    if (value.endsWith(QLatin1String("px")))
        value.chop(2);
    bool ok = false;
    const double amount = value.toDouble(&ok);
    return ok ? amount : fallback;
}

} // namespace

Painter::Painter(const Options &options)
    : m_options(options)
{
}

QColor Painter::canvasColor(Box *root) const
{
    // The canvas takes the background of the root element, falling back to the
    // body's, which is the rule browsers implement for the "canvas background".
    if (root) {
        if (root->style() && root->style()->backgroundColor.alpha() > 0)
            return root->style()->backgroundColor;
        for (const auto &child : root->children()) {
            if (child->element() && child->element()->isTag(QStringLiteral("body"))
                && child->style() && child->style()->backgroundColor.alpha() > 0) {
                return child->style()->backgroundColor;
            }
        }
    }
    return m_options.defaultBackground;
}

QRectF Painter::viewportRect(const Box *box) const
{
    QRectF rect = box->borderBox();
    rect.translate(-m_options.scrollX, -m_options.scrollY);
    return rect;
}

QImage Painter::renderToImage(Box *root, int width, int height)
{
    return renderToImage(root, width, height, Options());
}

QImage Painter::renderToImage(Box *root, int width, int height, const Options &options)
{
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    Painter painter(options);
    image.fill(painter.canvasColor(root));

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    painter.paint(&p, root);
    p.end();

    return image;
}

void Painter::paint(QPainter *painter, Box *root)
{
    if (!painter || !root)
        return;

    m_currentTextColor = m_options.defaultTextColor;
    paintBox(painter, root);

    if (m_options.showBoxModel) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(QColor(0, 160, 255, 160), 1, Qt::DashLine));
        painter->drawRect(viewportRect(root));
    }
}

void Painter::paintBox(QPainter *painter, Box *box)
{
    if (!box)
        return;

    const css::ComputedStyle *style = box->style();
    const QRectF rect = viewportRect(box);

    // Cull boxes entirely outside the viewport: long pages have far more boxes
    // than pixels, and this keeps painting proportional to what is visible.
    // An empty clip rect means the painter has no clip set, in which case
    // nothing is culled.
    const QRectF clip = painter->clipBoundingRect();
    if (!clip.isEmpty()
        && (rect.bottom() < clip.top() - 1 || rect.top() > clip.bottom() + 1
            || rect.right() < clip.left() - 1 || rect.left() > clip.right() + 1)) {
        return;
    }

    if (style && !style->visible)
        return;

    // Anonymous boxes and line boxes carry no appearance of their own; they
    // exist to hold the content that does, so only their children matter.
    if (box->type() == Box::Type::Anonymous || box->type() == Box::Type::Line) {
        for (const auto &child : box->children())
            paintBox(painter, child.get());
        return;
    }

    if (box->type() == Box::Type::Bullet) {
        paintBullet(painter, box);
        return;
    }

    if (box->type() == Box::Type::Text) {
        paintText(painter, box);
        return;
    }

    // Background, then border, then children, then the model overlay: painter's
    // algorithm with the box's own content on top.
    paintBackground(painter, box);
    paintBorders(painter, box);

    const QColor savedColor = m_currentTextColor;

    for (const auto &child : box->children())
        paintBox(painter, child.get());

    m_currentTextColor = savedColor;

    if (m_options.showBoxModel)
        paintBoxModelOverlay(painter, box);
}

void Painter::paintBackground(QPainter *painter, Box *box)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    const QRectF rect = viewportRect(box);

    if (style->backgroundColor.alpha() > 0) {
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(style->backgroundColor);

        const double radius = parseRadius(style->borderRadius, 0);
        if (radius > 0) {
            // The radius is clamped to half the smaller side, so a large value
            // produces a pill rather than an inverted shape.
            const double r = qMin(radius, qMin(rect.width(), rect.height()) / 2.0);
            painter->drawRoundedRect(rect, r, r);
        } else {
            painter->drawRect(rect);
        }
        painter->restore();
    }

    // A background image, when one loaded, is drawn over the colour.
    if (!style->backgroundImage.isEmpty() && box->intrinsicSize().isValid()
        && !box->intrinsicSize().isEmpty()) {
        // The image loader attaches decoded pixmaps through the box tree; the
        // position is resolved from the background-position when it is simple.
        QImage image(style->backgroundImage);
        if (!image.isNull()) {
            painter->drawImage(rect, image);
        }
    }
}

void Painter::paintBorders(QPainter *painter, Box *box)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    const QRectF rect = viewportRect(box);

    struct Side
    {
        const QString *style;
        double width;
        const QColor *color;
    };

    const Side sides[4] = {
        {&style->borderTopStyle, style->borderTopWidth, &style->borderTopColor},
        {&style->borderRightStyle, style->borderRightWidth, &style->borderRightColor},
        {&style->borderBottomStyle, style->borderBottomWidth, &style->borderBottomColor},
        {&style->borderLeftStyle, style->borderLeftWidth, &style->borderLeftColor},
    };

    if (style->borderTopStyle == QLatin1String("none")
        && style->borderRightStyle == QLatin1String("none")
        && style->borderBottomStyle == QLatin1String("none")
        && style->borderLeftStyle == QLatin1String("none")) {
        return;
    }

    painter->save();
    painter->setBrush(Qt::NoBrush);

    for (int i = 0; i < 4; ++i) {
        const Side &side = sides[i];
        if (*side.style == QLatin1String("none") || *side.style == QLatin1String("hidden")
            || side.width <= 0) {
            continue;
        }

        QPen pen(*side.color, side.width);

        if (*side.style == QLatin1String("dotted"))
            pen.setStyle(Qt::DotLine);
        else if (*side.style == QLatin1String("dashed"))
            pen.setStyle(Qt::DashLine);
        else if (*side.style == QLatin1String("double"))
            pen.setStyle(Qt::SolidLine), pen.setWidthF(qMax(1.0, side.width));
        else if (*side.style == QLatin1String("groove") || *side.style == QLatin1String("ridge")
                 || *side.style == QLatin1String("inset") || *side.style == QLatin1String("outset")) {
            // The 3D styles are approximated with a darker or lighter solid
            // line, which reads the same at typical border widths.
            QColor adjusted = *side.color;
            const bool darker = *side.style == QLatin1String("groove")
                || *side.style == QLatin1String("inset");
            adjusted = darker ? adjusted.darker(150) : adjusted.lighter(150);
            pen.setColor(adjusted);
        }

        painter->setPen(pen);

        // Each side is drawn as a line along its edge of the border box.
        const double half = side.width / 2.0;
        switch (i) {
        case 0:
            painter->drawLine(QPointF(rect.left(), rect.top() + half),
                              QPointF(rect.right(), rect.top() + half));
            break;
        case 1:
            painter->drawLine(QPointF(rect.right() - half, rect.top()),
                              QPointF(rect.right() - half, rect.bottom()));
            break;
        case 2:
            painter->drawLine(QPointF(rect.left(), rect.bottom() - half),
                              QPointF(rect.right(), rect.bottom() - half));
            break;
        case 3:
            painter->drawLine(QPointF(rect.left() + half, rect.top()),
                              QPointF(rect.left() + half, rect.bottom()));
            break;
        default:
            break;
        }
    }

    painter->restore();
}

void Painter::paintText(QPainter *painter, Box *box)
{
    if (box->text().isEmpty())
        return;

    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    const QFont font = LayoutEngine::fontFor(style);
    const QRectF rect = viewportRect(box);

    painter->save();
    painter->setFont(font);
    painter->setPen(style->color.isValid() ? style->color : m_currentTextColor);
    painter->setClipRect(rect.adjusted(0, -2, 2, 2));

    // The text is drawn at the box's baseline: the box top is the ascent line
    // for the fragment, so drawing at the top with Qt's own baseline handling
    // lands the glyphs where the line builder measured them.
    const QFontMetricsF metrics(font);
    painter->drawText(QPointF(rect.left(), rect.top() + metrics.ascent()), box->text());

    if (style->textDecoration.contains(QLatin1String("underline"))) {
        const double y = rect.top() + metrics.ascent() + 1.5;
        painter->setPen(QPen(painter->pen().color(), 1));
        painter->drawLine(QPointF(rect.left(), y),
                          QPointF(rect.left() + metrics.horizontalAdvance(box->text()), y));
    }
    if (style->textDecoration.contains(QLatin1String("line-through"))) {
        const double y = rect.top() + metrics.ascent() - metrics.xHeight() / 2;
        painter->setPen(QPen(painter->pen().color(), 1));
        painter->drawLine(QPointF(rect.left(), y),
                          QPointF(rect.left() + metrics.horizontalAdvance(box->text()), y));
    }

    painter->restore();

    if (!m_options.selection.isNull()) {
        const QRectF selectionRect = rect.intersected(
            m_options.selection.translated(-m_options.scrollX, -m_options.scrollY));
        if (!selectionRect.isEmpty()) {
            painter->save();
            painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter->fillRect(selectionRect, m_options.selectionColor);
            painter->restore();
        }
    }
}

void Painter::paintBullet(QPainter *painter, Box *box)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    // The marker sits just inside the list item's own left padding, aligned
    // with the first line of text.
    const QFont font = LayoutEngine::fontFor(style);
    const QFontMetricsF metrics(font);

    Box *parentBox = box->parent();
    QRectF anchor = parentBox ? viewportRect(parentBox) : viewportRect(box);

    painter->save();
    painter->setFont(font);
    painter->setPen(style->color.isValid() ? style->color : m_currentTextColor);

    const double markerWidth = metrics.horizontalAdvance(box->text());
    const double x = anchor.left() - markerWidth - 7; // marker gap, as browsers use
    const double y = anchor.top() + metrics.ascent();

    // Clipping to the viewport keeps markers of off-screen items cheap.
    if (y > painter->clipBoundingRect().bottom() + 20
        || y < painter->clipBoundingRect().top() - 20) {
        painter->restore();
        return;
    }

    painter->drawText(QPointF(x, y), box->text());
    painter->restore();
}

void Painter::paintBoxModelOverlay(QPainter *painter, Box *box)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    const QRectF rect = viewportRect(box);
    painter->save();
    painter->setBrush(Qt::NoBrush);

    // Content (blue), padding (green) and margin (orange) follow the DevTools
    // colour convention so the overlay is immediately readable.
    painter->setPen(QPen(QColor(0, 160, 255, 180), 1, Qt::DashLine));
    painter->drawRect(rect);

    const QRectF padding = box->paddingBox();
    if (padding.isValid() && padding != box->borderBox()) {
        QRectF paddingInViewport = padding;
        paddingInViewport.translate(-m_options.scrollX, -m_options.scrollY);
        painter->setPen(QPen(QColor(64, 190, 64, 180), 1, Qt::DashLine));
        painter->drawRect(paddingInViewport);
    }

    const QRectF margin = box->marginBox();
    if (margin.isValid() && margin != box->borderBox()) {
        QRectF marginInViewport = margin;
        marginInViewport.translate(-m_options.scrollX, -m_options.scrollY);
        painter->setPen(QPen(QColor(230, 150, 0, 180), 1, Qt::DashLine));
        painter->drawRect(marginInViewport);
    }

    painter->restore();
}

} // namespace oqb::renderer
