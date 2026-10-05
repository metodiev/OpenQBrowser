#include "renderer/Layout.h"

#include <QFontMetricsF>

#include <algorithm>
#include <functional>

namespace oqb::renderer {
namespace {

/// Reported once when layout reaches its nesting limit. A page that trips this
/// is almost certainly generated rather than written, and the honest outcome is
/// a truncated page plus a warning rather than a crash.
const QString kDepthWarning
    = QStringLiteral("Layout stopped at the nesting limit of %1 levels; deeper content "
                     "was not laid out")
          .arg(LayoutEngine::kMaxLayoutDepth);

/// The font stack a generic family resolves to, used when a page names no
/// family the system can provide.
QStringList familiesFor(const QString &family)
{
    if (family == QLatin1String("monospace"))
        return {QStringLiteral("Menlo"), QStringLiteral("Courier New"), QStringLiteral("monospace")};
    if (family == QLatin1String("serif"))
        return {QStringLiteral("Times New Roman"), QStringLiteral("Georgia"),
                QStringLiteral("serif")};
    if (family == QLatin1String("sans-serif"))
        return {QStringLiteral("Helvetica Neue"), QStringLiteral("Arial"),
                QStringLiteral("sans-serif")};
    if (family == QLatin1String("cursive"))
        return {QStringLiteral("Comic Sans MS"), QStringLiteral("cursive")};
    if (family == QLatin1String("fantasy"))
        return {QStringLiteral("Impact"), QStringLiteral("fantasy")};
    if (family == QLatin1String("system-ui"))
        return {QStringLiteral("Helvetica Neue"), QStringLiteral("Arial"),
                QStringLiteral("sans-serif")};
    return {family};
}

/// The bullet text a list item shows, per list-style-type.
QString markerFor(const QString &type, int index)
{
    if (type == QLatin1String("none"))
        return {};
    if (type == QLatin1String("disc"))
        return QStringLiteral("\u2022");
    if (type == QLatin1String("circle"))
        return QStringLiteral("\u25E6");
    if (type == QLatin1String("square"))
        return QStringLiteral("\u25AA");
    if (type == QLatin1String("decimal"))
        return QString::number(index) + u'.';
    if (type.startsWith(QLatin1String("lower-alpha")) || type.startsWith(QLatin1String("lower-latin")))
        return QString(QChar(u'a' + (index - 1) % 26)) + u'.';
    if (type.startsWith(QLatin1String("upper-alpha")) || type.startsWith(QLatin1String("upper-latin")))
        return QString(QChar(u'A' + (index - 1) % 26)) + u'.';
    if (type.contains(QLatin1String("roman"))) {
        static const struct
        {
            int value;
            const char *numeral;
        } kNumerals[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"},
                         {90, "xc"},  {50, "l"},   {40, "xl"}, {10, "x"},   {9, "ix"},
                         {5, "v"},    {4, "iv"},   {1, "i"}};
        QString out;
        int remaining = qBound(1, index, 3999);
        for (const auto &numeral : kNumerals) {
            while (remaining >= numeral.value) {
                out += QString::fromLatin1(numeral.numeral);
                remaining -= numeral.value;
            }
        }
        if (type.startsWith(QLatin1String("upper")))
            out = out.toUpper();
        return out + u'.';
    }
    return QStringLiteral("\u2022");
}

} // namespace

Box *LayoutResult::hitTest(double x, double y) const
{
    Box *best = nullptr;
    std::function<void(Box *)> visit = [&](Box *box) {
        if (!box)
            return;
        for (const auto &child : box->children())
            visit(child.get());

        const QRectF rect = box->borderBox();
        if (rect.contains(x, y) && box->type() != Box::Type::Anonymous
            && box->type() != Box::Type::Line) {
            // Later visits are deeper, so keeping the last match finds the most
            // specific box under the point.
            if (!best || box->borderBox().width() <= best->borderBox().width())
                best = box;
        }
    };
    for (Box *line : lineBoxes)
        visit(line);
    return best;
}

QFont LayoutEngine::fontFor(const css::ComputedStyle *style)
{
    QFont font;
    font.setPointSizeF(style ? qMax(1.0, style->fontSize) : 16.0);

    QStringList families;
    if (style && !style->fontFamilies.isEmpty()) {
        for (const QString &family : style->fontFamilies)
            families.append(familiesFor(family));
    } else {
        families = familiesFor(QStringLiteral("sans-serif"));
    }
    font.setFamilies(families);

    if (style) {
        font.setWeight(QFont::Weight(qBound(1, style->fontWeight, 1000)));
        font.setItalic(style->italic);
        if (!qFuzzyIsNull(style->letterSpacing))
            font.setLetterSpacing(QFont::AbsoluteSpacing, style->letterSpacing);
        if (!qFuzzyIsNull(style->wordSpacing))
            font.setWordSpacing(style->wordSpacing);
    }

    return font;
}

void LayoutEngine::setViewport(double width, double height)
{
    m_viewportWidth = qMax(1.0, width);
    m_viewportHeight = qMax(1.0, height);
}

LayoutEngine::Edges LayoutEngine::paddingOf(const Box *box, double containingBlockWidth) const
{
    Edges edges;
    const css::ComputedStyle *style = box->style();
    if (!style)
        return edges;
    edges.top = style->paddingTop.resolve(containingBlockWidth);
    edges.right = style->paddingRight.resolve(containingBlockWidth);
    edges.bottom = style->paddingBottom.resolve(containingBlockWidth);
    edges.left = style->paddingLeft.resolve(containingBlockWidth);
    return edges;
}

LayoutEngine::Edges LayoutEngine::borderOf(const Box *box) const
{
    Edges edges;
    const css::ComputedStyle *style = box->style();
    if (!style)
        return edges;
    edges.top = style->borderTopWidth;
    edges.right = style->borderRightWidth;
    edges.bottom = style->borderBottomWidth;
    edges.left = style->borderLeftWidth;
    return edges;
}

double LayoutEngine::resolveUsedWidth(const Box *box, double containingBlockWidth, bool *isAuto) const
{
    const css::ComputedStyle *style = box->style();
    if (!style) {
        if (isAuto)
            *isAuto = true;
        return containingBlockWidth;
    }

    if (isAuto)
        *isAuto = style->width.isAuto();
    if (style->width.isAuto())
        return containingBlockWidth;

    double width = style->width.resolve(containingBlockWidth);

    if (style->minWidth.isLength())
        width = qMax(width, style->minWidth.value);
    if (style->maxWidth.isLength())
        width = qMin(width, style->maxWidth.value);
    else if (style->maxWidth.isPercentage())
        width = qMin(width, style->maxWidth.resolve(containingBlockWidth));

    return qMax(0.0, width);
}

double LayoutEngine::resolveUsedHeight(const Box *box, const Context &context) const
{
    const css::ComputedStyle *style = box->style();
    if (!style || style->height.isAuto())
        return -1;

    const double base = context.availableHeight >= 0 ? context.availableHeight : 0;
    double height = style->height.resolve(base);

    if (style->minHeight.isLength())
        height = qMax(height, style->minHeight.value);
    if (style->maxHeight.isLength())
        height = qMin(height, style->maxHeight.value);

    return qMax(0.0, height);
}

void LayoutEngine::layoutReplaced(Box *box, const Context &context)
{
    QSizeF intrinsic = box->intrinsicSize();
    if (!intrinsic.isValid() || intrinsic.isEmpty()) {
        const dom::Element *element = box->element();
        if (element && element->isTag(QStringLiteral("input")))
            intrinsic = QSizeF(173, 21);
        else if (element && element->isTag(QStringLiteral("textarea")))
            intrinsic = QSizeF(173, 64);
        else if (element && element->isTag(QStringLiteral("iframe")))
            intrinsic = QSizeF(300, 150);
        else if (element && element->isTag(QStringLiteral("canvas")))
            intrinsic = QSizeF(300, 150);
        else if (element && element->isTag(QStringLiteral("video")))
            intrinsic = QSizeF(300, 150);
        else
            intrinsic = QSizeF(0, 0);
    }

    bool widthAuto = true;
    const double specifiedWidth = resolveUsedWidth(box, context.availableWidth, &widthAuto);
    const double specifiedHeight = resolveUsedHeight(box, context);

    double width = specifiedWidth;
    double height = specifiedHeight;

    // CSS 2.2 §10.3.2: when one dimension is auto, the intrinsic ratio decides
    // it, and when both are auto the intrinsic size is used directly.
    if (widthAuto && specifiedHeight < 0) {
        width = intrinsic.width();
        height = intrinsic.height();
    } else if (widthAuto) {
        width = intrinsic.height() > 0 ? specifiedHeight * intrinsic.width() / intrinsic.height()
                                       : intrinsic.width();
        height = specifiedHeight;
    } else if (specifiedHeight < 0) {
        height = intrinsic.width() > 0 ? width * intrinsic.height() / intrinsic.width()
                                       : intrinsic.height();
    }

    box->setSize(qMax(0.0, width), qMax(0.0, height));
    box->setContentBox(QRectF(0, 0, box->width(), box->height()));
}

double LayoutEngine::shrinkToFitWidth(Box *box, const Context &context)
{
    // Measure the content as if it had unlimited room, then clamp. Text is
    // measured through the font metrics rather than by re-laying it out, which
    // keeps this a single pass over the subtree.
    double preferred = 0;

    std::function<void(const Box *, double)> measure = [&](const Box *node, double indent) {
        if (!node)
            return;

        if (node->isText()) {
            const css::ComputedStyle *style = node->style() ? node->style() : box->style();
            const QFont font = fontFor(style);
            const QFontMetricsF metrics(font);
            preferred = qMax(preferred, indent + metrics.horizontalAdvance(node->text()));
            return;
        }

        if (node->isReplaced() || node->type() == Box::Type::InlineBlock) {
            preferred = qMax(preferred, indent + node->borderBox().width());
            return;
        }

        const css::ComputedStyle *style = node->style() ? node->style() : box->style();
        double ownIndent = indent;
        if (style) {
            ownIndent += style->paddingLeft.resolve(context.availableWidth)
                + style->borderLeftWidth + style->marginLeft.resolve(context.availableWidth);
        }
        for (const auto &child : node->children())
            measure(child.get(), ownIndent);
    };

    for (const auto &child : box->children())
        measure(child.get(), 0);

    // The measured width is a content width; the box's own padding and border
    // are added here, because they are part of the outer width the line needs.
    const css::ComputedStyle *style = box->style();
    if (style) {
        preferred += style->paddingLeft.resolve(context.availableWidth)
            + style->paddingRight.resolve(context.availableWidth) + style->borderLeftWidth
            + style->borderRightWidth;
    }

    return qMin(preferred, context.availableWidth);
}

/// Moves a box so its border box starts at (x, y), carrying its subtree, which
/// is what makes an atomic inline box keep its internal layout.
void LayoutEngine::placeBoxAt(Box *box, double x, double y)
{
    const QRectF current = box->borderBox();
    const double dx = x - current.x();
    const double dy = y - current.y();
    if (!qFuzzyIsNull(dx) || !qFuzzyIsNull(dy))
        box->translate(dx, dy);
}

double LayoutEngine::trailingMarginOf(const Box *box, const Context &context) const
{
    const css::ComputedStyle *style = box->style();
    if (!style || style->marginBottom.isAuto())
        return 0;
    return style->marginBottom.resolve(context.availableWidth);
}

double LayoutEngine::layoutBlockChildren(Box *box, const Context &context, double *lastMargin)
{
    const css::ComputedStyle *style = box->style();
    double y = context.contentY;
    double collapsedMargin = 0;

    for (const auto &childPtr : box->children()) {
        Box *child = childPtr.get();
        if (!child->isBlockLevel())
            continue;

        const css::ComputedStyle *childStyle = child->style();
        if (!childStyle || !childStyle->generatesBox())
            continue;

        // Vertical margins between adjacent siblings collapse: the gap between
        // two boxes is the larger of the two margins, never their sum
        // (CSS 2.2 §8.3.1). The gap is therefore computed here, and the child is
        // laid out with no margin added on top of it.
        const double childMarginTop = childStyle->marginTop.isAuto()
            ? 0
            : childStyle->marginTop.resolve(context.availableWidth);
        const double gap = qMax(collapsedMargin, childMarginTop);

        // layoutBlock returns the y past the child's own bottom margin, so the
        // collapsed gap is applied before it and the child's border box is
        // placed directly after it.
        y += gap;
        y = layoutBlock(child, context, y) - trailingMarginOf(child, context);

        collapsedMargin = childStyle->marginBottom.isAuto()
            ? 0
            : childStyle->marginBottom.resolve(context.availableWidth);
    }

    if (lastMargin)
        *lastMargin = collapsedMargin;

    Q_UNUSED(style);

    // The trailing margin of the last child collapses with the parent's bottom
    // margin, so it is reported separately rather than added here.
    return y - context.contentY;
}

double LayoutEngine::layoutInlineRun(Box *container, const Context &context,
                                    double *widestLine)
{
    // ------------------------------------------------------------ measuring
    struct Fragment
    {
        /// The DOM node the text came from, used only as an identity for
        /// hit testing. The Box it belonged to is released before the line
        /// boxes are built, so it is never dereferenced here.
        dom::Node *node = nullptr;
        const css::ComputedStyle *style = nullptr;
        QString text;
        bool spaceBefore = false;
        bool breakBefore = false; ///< A <br> forces a new line.
        bool isReplaced = false;
        /// True for an inline-block: a box that is atomic to the line but keeps
        /// its own subtree, which is moved into the line rather than re-created.
        bool isAtomicBox = false;
        /// Index into the replaced-box list, for re-attaching the box in place.
        size_t replacedIndex = 0;
        double width = 0;
        /// The fragment's natural height, i.e. the font's line box for text.
        double height = 0;
        double baseline = 0;
        double lineHeight = 0;
    };

    QList<Fragment> fragments;
    size_t inlineLayoutReplacedIndex = 0;
    double widestReplaced = 0;

    std::function<void(Box *, const css::ComputedStyle *)> collect
        = [&](Box *box, const css::ComputedStyle *inherited) {
              const css::ComputedStyle *style = box->style() ? box->style() : inherited;
              if (!style)
                  return;

              // <br> is a forced break; it carries no visible content.
              if (box->element() && box->element()->isTag(QStringLiteral("br"))) {
                  Fragment fragment;
                  fragment.breakBefore = true;
                  fragment.height = style->lineHeight;
                  fragment.lineHeight = style->lineHeight;
                  fragments.append(fragment);
                  return;
              }

              if (box->isReplaced()) {
                  // A replaced box is laid out during collection and its size is
                  // captured as plain numbers, so the fragment stays valid after
                  // the inline children are released.
                  Context replacedContext = context;
                  layoutReplaced(box, replacedContext);
                  Fragment fragment;
                  fragment.style = style;
                  fragment.isReplaced = true;
                  fragment.replacedIndex = inlineLayoutReplacedIndex++;
                  fragment.width = box->width();
                  fragment.height = qMax(box->height(), style->lineHeight);
                  fragment.baseline = box->height();
                  fragment.lineHeight = fragment.height;
                  widestReplaced = qMax(widestReplaced, fragment.width);
                  fragments.append(fragment);
                  return;
              }

              if (box->isText()) {
                  const QFont font = fontFor(style);
                  const QFontMetricsF metrics(font);
                  const double naturalHeight = metrics.height();
                  const double lineHeight = style->lineHeight > 0 ? style->lineHeight
                                                                  : naturalHeight;

                  // Words are the units a line can break on; the spaces between
                  // them are the break opportunities, which is why they are
                  // tracked as a flag rather than as content.
                  const QStringList words = box->text().split(u' ');
                  for (int i = 0; i < words.size(); ++i) {
                      const QString &word = words.at(i);
                      if (word.isEmpty())
                          continue;
                      Fragment fragment;
                      fragment.node = box->node();
                      fragment.style = style;
                      fragment.text = word;
                      fragment.spaceBefore = i > 0;
                      fragment.width = metrics.horizontalAdvance(word);
                      fragment.height = lineHeight;
                      fragment.baseline = metrics.ascent();
                      fragment.lineHeight = lineHeight;
                      fragments.append(fragment);
                  }
                  return;
              }

              // An inline-block is atomic to the line: it is sized as a block
              // and then treated as a single placeholder, so its background,
              // border and inner layout survive.
              if (box->type() == Box::Type::InlineBlock) {
                  Context blockContext = context;
                  // An auto-width inline-block shrinks to fit its content,
                  // which is why a badge or a tag is only as wide as its text.
                  const css::ComputedStyle *blockStyle = box->style();
                  if (blockStyle && blockStyle->width.isAuto()) {
                      // The measurement is an outer width, which is exactly what
                      // the block layout treats its available width as.
                      blockContext.availableWidth
                          = qMax(0.0, shrinkToFitWidth(box, blockContext));
                  }
                  layoutBlock(box, blockContext, 0);
                  Fragment fragment;
                  fragment.style = style;
                  fragment.node = box->node();
                  fragment.isAtomicBox = true;
                  fragment.replacedIndex = inlineLayoutReplacedIndex++;
                  fragment.width = box->borderBox().width();
                  fragment.height = qMax(box->borderBox().height(), style->lineHeight);
                  fragment.baseline = box->borderBox().height();
                  fragment.lineHeight = fragment.height;
                  widestReplaced = qMax(widestReplaced, fragment.width);
                  fragments.append(fragment);
                  return;
              }

              for (const auto &child : box->children())
                  collect(child.get(), style);
          };

    collect(container, container->style());

    // The inline children have now been measured into fragments, which carry the
    // text, the resolved style and the geometry. Keeping them around would leave
    // unpositioned text boxes at the container's origin for the painter to draw,
    // so they are released below. Replaced boxes are the exception: they hold
    // real content and are re-attached afterwards, in place.
    std::vector<Box *> replacedPointers;
    std::function<void(Box *)> findReplaced = [&](Box *parent) {
        for (const auto &child : parent->children()) {
            if (child->isReplaced() || child->type() == Box::Type::InlineBlock)
                replacedPointers.push_back(child.get());
            else
                findReplaced(child.get());
        }
    };
    findReplaced(container);

    std::vector<std::unique_ptr<Box>> replacedBoxes;
    for (Box *box : replacedPointers) {
        if (Box *parent = box->parent()) {
            if (std::unique_ptr<Box> owned = parent->detachChild(box))
                replacedBoxes.push_back(std::move(owned));
        }
    }
    container->clearChildren();

    // ------------------------------------------------------------ line building
    double y = context.contentY;
    double totalWidth = 0;
    double preferredWidth = 0;
    double lineWidth = 0;
    double lineAscent = 0;
    double lineHeight = 0;
    int lineStartFragment = 0;

    // Text fragments are laid out row by row; the boxes that hold them are
    // attached to the container so painting and hit testing can find them.
    const auto flushLine = [&](int endIndex) {
        const double neededHeight = qMax(lineHeight, 1.0);
        if (endIndex <= lineStartFragment) {
            // A line with no content still advances, which is what an empty
            // paragraph or a <br> requires.
            y += neededHeight;
            lineStartFragment = endIndex;
            lineAscent = 0;
            lineHeight = 0;
            lineWidth = 0;
            return;
        }

        auto lineOwner = std::make_unique<Box>(Box::Type::Line, nullptr, container->style());
        Box *line = container->appendChild(std::move(lineOwner));

        // The text fragments on this line are attached to it with absolute
        // geometry, so the painter can draw them directly.
        double x = context.contentX;
        for (int i = lineStartFragment; i < endIndex && i < fragments.size(); ++i) {
            const Fragment &fragment = fragments.at(i);
            if (fragment.breakBefore)
                continue;

            const double spaceWidth = fragment.spaceBefore
                ? QFontMetricsF(fontFor(fragment.style)).horizontalAdvance(u' ')
                : 0.0;
            x += spaceWidth;

            if (fragment.isAtomicBox || fragment.isReplaced) {
                // Both atomic inline-blocks and replaced content keep a real box
                // of their own; the line takes it back from the detached list
                // and moves it into place. Neither is re-created, so an
                // inline-block keeps the subtree that was laid out for it.
                if (fragment.replacedIndex < replacedBoxes.size()) {
                    std::unique_ptr<Box> box = std::move(replacedBoxes[fragment.replacedIndex]);
                    if (box) {
                        // Replaced content is centred in the line; an inline
                        // block sits with its own baseline on the line's.
                        const double offset = fragment.isAtomicBox
                            ? 0.0
                            : qMax(0.0, (neededHeight - box->height()) / 2);
                        // The whole subtree moves with the box, so an
                        // inline-block's contents stay inside their box.
                        placeBoxAt(box.get(), x, y + offset);
                        line->appendChild(std::move(box));
                    }
                }
                x += fragment.width;
                continue;
            }

            auto textBox = std::make_unique<Box>(Box::Type::Text, fragment.node, fragment.style);
            textBox->setText(fragment.text);
            // The baseline is aligned by placing the box so that the ascent
            // lands on the line's baseline, which is what keeps mixed font
            // sizes sitting on a common line.
            textBox->setPosition(x, y + qMax(0.0, lineAscent - fragment.baseline));
            textBox->setSize(fragment.width, fragment.height);
            textBox->setContentBox(QRectF(0, 0, fragment.width, fragment.height));
            line->appendChild(std::move(textBox));
            x += fragment.width;
        }

        line->setPosition(context.contentX, y);
        // The line box itself is only as wide as its content, which is what the
        // painter and hit testing expect; the container's width is a layout
        // concern, not a property of the line.
        const double usedWidth = qMin(lineWidth, context.availableWidth);
        line->setSize(usedWidth, neededHeight);
        line->setContentBox(QRectF(0, 0, usedWidth, neededHeight));
        totalWidth = qMax(totalWidth, usedWidth);
        preferredWidth = qMax(preferredWidth, lineWidth);
        m_result.lineBoxes.push_back(line);

        y += neededHeight;
        lineStartFragment = endIndex;
        lineAscent = 0;
        lineHeight = 0;
        lineWidth = 0;
    };

    for (int i = 0; i < fragments.size(); ++i) {
        const Fragment &fragment = fragments.at(i);

        if (fragment.breakBefore) {
            flushLine(i);
            continue;
        }

        const double spaceWidth = fragment.spaceBefore
            ? QFontMetricsF(fontFor(fragment.style)).horizontalAdvance(u' ')
            : 0.0;
        const bool mustWrap = lineWidth > 0 && (lineWidth + spaceWidth + fragment.width) > context.availableWidth;

        if (mustWrap)
            flushLine(i);

        lineWidth += (lineWidth > 0 ? spaceWidth : 0.0) + fragment.width;
        lineAscent = qMax(lineAscent, fragment.baseline);
        lineHeight = qMax(lineHeight, fragment.lineHeight);
    }

    flushLine(static_cast<int>(fragments.size()));

    if (widestLine)
        *widestLine = preferredWidth;

    Q_UNUSED(widestReplaced);
    Q_UNUSED(totalWidth);
    return y - context.contentY;
}

double LayoutEngine::layoutBlock(Box *box, const Context &context, double borderTopY)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return borderTopY;

    // Beyond this depth the box is given its position but no content height, so
    // the document still lays out and the limit shows up as content that is
    // clipped rather than as a crash.
    if (context.depth >= kMaxLayoutDepth) {
        if (!m_result.warnings.contains(kDepthWarning))
            m_result.warnings.append(kDepthWarning);
        box->setPosition(context.contentX, borderTopY);
        box->setSize(context.availableWidth, 0);
        box->setContentBox(QRectF(0, 0, context.availableWidth, 0));
        return borderTopY;
    }

    // An anonymous box exists only to hold content. It borrows its parent's
    // style so that its text inherits correctly, but the parent's own box
    // decoration belongs to the parent, not to this wrapper.
    const bool anonymous = box->type() == Box::Type::Anonymous;

    const Edges padding = anonymous ? Edges() : paddingOf(box, context.availableWidth);
    const Edges border = anonymous ? Edges() : borderOf(box);

    // ------------------------------------------------------------- width
    bool widthAuto = false;
    const double specifiedWidth = anonymous
        ? context.availableWidth
        : resolveUsedWidth(box, context.availableWidth, &widthAuto);
    if (anonymous)
        widthAuto = true;

    // Auto horizontal margins split whatever space is left, which is how a
    // block with a specified width is centred.
    double marginLeft = (anonymous || style->marginLeft.isAuto())
        ? 0
        : style->marginLeft.resolve(context.availableWidth);
    double marginRight = (anonymous || style->marginRight.isAuto())
        ? 0
        : style->marginRight.resolve(context.availableWidth);

    const double outerNonContent
        = padding.left + padding.right + border.left + border.right + marginLeft + marginRight;

    double contentWidth = specifiedWidth;
    if (widthAuto)
        contentWidth = qMax(0.0, context.availableWidth - outerNonContent);

    if (!widthAuto) {
        double leftover = context.availableWidth - outerNonContent - contentWidth;
        if (leftover > 0) {
            if (style->marginLeft.isAuto() && style->marginRight.isAuto()) {
                marginLeft += leftover / 2;
                marginRight += leftover / 2;
            } else if (style->marginLeft.isAuto()) {
                marginLeft += leftover;
            } else if (style->marginRight.isAuto()) {
                marginRight += leftover;
            }
        }
    }

    const double contentX = context.contentX + marginLeft + border.left + padding.left;
    const double contentY = borderTopY + border.top + padding.top;

    // ------------------------------------------------------------ children
    Context inner;
    inner.depth = context.depth + 1;
    inner.contentX = contentX;
    inner.contentY = contentY;
    inner.availableWidth = contentWidth;
    inner.availableHeight = -1;

    // A list item shows its marker as a bullet box beside its first line. The
    // marker is created here, once per item, before the content is laid out.
    // Anonymous boxes inherit their parent's style, so they are excluded
    // explicitly: only the item itself owns a marker.
    if (style->isListItem && box->type() == Box::Type::Block
        && style->listStylePosition == QLatin1String("outside")) {
        static thread_local QHash<const Box *, int> s_listCounters;
        const int index = s_listCounters.value(box->parent(), 0) + 1;
        s_listCounters.insert(box->parent(), index);
        const QString marker = markerFor(style->listStyleType, index);
        if (!marker.isEmpty()) {
            auto bullet = std::make_unique<Box>(Box::Type::Bullet, nullptr, style);
            bullet->setText(marker);
            box->appendChild(std::move(bullet));
        }
    }

    double contentHeight = 0;
    double trailingMargin = 0;

    if (box->isReplaced()) {
        Context replacedContext = inner;
        layoutReplaced(box, replacedContext);
        contentHeight = box->height();
    } else {
        // The container either stacks block children or holds line boxes, never
        // both: the tree builder wraps mixed content in anonymous blocks. Doing
        // the work twice would add every list marker twice.
        bool hasBlockChild = false;
        for (const auto &child : box->children()) {
            if (child->isBlockLevel())
                hasBlockChild = true;
        }

        if (hasBlockChild)
            contentHeight = layoutBlockChildren(box, inner, &trailingMargin);
        else
            contentHeight = layoutInlineRun(box, inner);
    }

    // A specified height wins over the content height (CSS 2.2 §10.6.3).
    const double specifiedHeight = resolveUsedHeight(box, context);
    if (specifiedHeight >= 0)
        contentHeight = qMax(0.0, specifiedHeight - padding.top - padding.bottom - border.top
                                       - border.bottom);

    // ------------------------------------------------------------- geometry
    const double borderBoxWidth = contentWidth + padding.left + padding.right + border.left
        + border.right;
    const double borderBoxHeight = contentHeight + padding.top + padding.bottom + border.top
        + border.bottom;

    box->setPosition(context.contentX + marginLeft, borderTopY);
    box->setSize(borderBoxWidth, borderBoxHeight);
    box->setContentBox(QRectF(0, 0, contentWidth, contentHeight));

    // The bottom margin collapses with the parent's, so it is not part of the
    // height returned to the caller; the caller adds the collapsed amount.
    return borderTopY + borderBoxHeight + trailingMargin;
}

LayoutResult LayoutEngine::layout(Box *root)
{
    m_result = LayoutResult();
    if (!root)
        return m_result;

    Context context;
    context.contentX = 0;
    context.contentY = 0;
    context.availableWidth = m_viewportWidth;
    context.availableHeight = -1;

    const css::ComputedStyle *style = root->style();
    const double marginTop = style && !style->marginTop.isAuto()
        ? style->marginTop.resolve(m_viewportWidth)
        : 0;

    layoutBlock(root, context, marginTop);

    // The document is as tall as the deepest content, which is not always the
    // root box itself: a child can overflow its parent when the parent has a
    // specified height, and a scrolling page must still reach that content.
    double contentBottom = root->y() + root->height();
    std::function<void(Box *)> measure = [&](Box *box) {
        contentBottom = qMax(contentBottom, box->borderBox().bottom());
        for (const auto &child : box->children())
            measure(child.get());
    };
    measure(root);

    m_result.documentWidth = qMax(m_viewportWidth, root->x() + root->width());
    m_result.documentHeight = qMax(contentBottom, m_viewportHeight);

    return m_result;
}

} // namespace oqb::renderer
