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

    // With border-box the specified width already includes the padding and
    // border, so they are subtracted to leave the content width the rest of
    // layout works in. This is what `* { box-sizing: border-box }` asks for, and
    // nearly every modern stylesheet sets it.
    if (style->boxSizing == QLatin1String("border-box")) {
        const Edges padding = paddingOf(box, containingBlockWidth);
        const Edges border = borderOf(box);
        width -= padding.left + padding.right + border.left + border.right;
    }

    // The minimum and maximum are applied to the content width, since that is
    // what this function returns. Under border-box they describe the border box,
    // so the same subtraction applies to them.
    const bool borderBox = style->boxSizing == QLatin1String("border-box");
    const Edges padding = paddingOf(box, containingBlockWidth);
    const Edges border = borderOf(box);
    const double furniture
        = borderBox ? padding.left + padding.right + border.left + border.right : 0.0;

    if (style->minWidth.isLength())
        width = qMax(width, style->minWidth.value - furniture);
    else if (style->minWidth.isPercentage())
        width = qMax(width, style->minWidth.resolve(containingBlockWidth) - furniture);

    if (style->maxWidth.isLength())
        width = qMin(width, style->maxWidth.value - furniture);
    else if (style->maxWidth.isPercentage())
        width = qMin(width, style->maxWidth.resolve(containingBlockWidth) - furniture);

    return qMax(0.0, width);
}

double LayoutEngine::resolveUsedHeight(const Box *box, const Context &context) const
{
    const css::ComputedStyle *style = box->style();
    if (!style || style->height.isAuto())
        return -1;

    const double base = context.availableHeight >= 0 ? context.availableHeight : 0;
    double height = style->height.resolve(base);

    // As with the width, a border-box height includes the padding and border, so
    // they come off to leave the content height this function reports.
    const bool borderBox = style->boxSizing == QLatin1String("border-box");
    const Edges padding = paddingOf(box, context.availableWidth);
    const Edges border = borderOf(box);
    const double furniture
        = borderBox ? padding.top + padding.bottom + border.top + border.bottom : 0.0;

    if (borderBox)
        height -= furniture;

    if (style->minHeight.isLength())
        height = qMax(height, style->minHeight.value - furniture);
    if (style->maxHeight.isLength())
        height = qMin(height, style->maxHeight.value - furniture);

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

// ------------------------------------------------------------------ flexbox

namespace {

/// A resolved margin on one edge of a flex item.
struct FlexMargin
{
    double value = 0;
    bool isAuto = false;
};

/// True when the container lays its items out horizontally.
bool isRowDirection(const QString &direction)
{
    return direction == QLatin1String("row") || direction == QLatin1String("row-reverse");
}

/// True when the container reverses the order of its items.
bool isReverseDirection(const QString &direction)
{
    return direction == QLatin1String("row-reverse")
        || direction == QLatin1String("column-reverse");
}

/// True when the container wraps onto multiple lines.
bool wrapsLines(const css::ComputedStyle *style)
{
    return style->flexWrap != QLatin1String("nowrap");
}

/// The gap between items on the main axis, which depends on the direction.
double mainGap(const css::ComputedStyle *style, bool rowDirection)
{
    const css::LengthOrAuto &gap = rowDirection ? style->columnGap : style->rowGap;
    return gap.isAuto() ? 0 : gap.resolve(0);
}

/// The gap between lines on the cross axis.
double crossGap(const css::ComputedStyle *style, bool rowDirection)
{
    const css::LengthOrAuto &gap = rowDirection ? style->rowGap : style->columnGap;
    return gap.isAuto() ? 0 : gap.resolve(0);
}

} // namespace

QString LayoutEngine::resolveAlignSelf(const FlexItem &item, const css::ComputedStyle *container)
{
    // The item's own align-self wins unless it says auto, in which case the
    // container's align-items applies. The result is never "auto".
    if (!item.style->alignSelf.isEmpty() && item.style->alignSelf != QLatin1String("auto"))
        return item.style->alignSelf;
    return container->alignItems;
}

double LayoutEngine::layoutFlexChildren(Box *box, const Context &context)
{
    const css::ComputedStyle *containerStyle = box->style();
    if (!containerStyle)
        return 0;

    const bool rowDirection = isRowDirection(containerStyle->flexDirection);
    const bool reverse = isReverseDirection(containerStyle->flexDirection);
    const double gapMain = mainGap(containerStyle, rowDirection);
    const double gapCross = crossGap(containerStyle, rowDirection);

    // The container's content box. For a row this is the available width; for a
    // column the main axis is vertical and its extent comes from the container's
    // height, which may be indefinite and then has to be measured from content.
    const double mainAvailable = rowDirection ? context.availableWidth
                                              : (context.availableHeight >= 0
                                                     ? context.availableHeight
                                                     : -1);
    const double crossAvailable
        = rowDirection ? (context.availableHeight >= 0 ? context.availableHeight : -1)
                       : context.availableWidth;

    // ------------------------------------------------------------ collect
    //
    // Items are collected in order, then sorted by `order`, which is what the
    // property is for. The sort is stable so that equal orders keep document
    // order, as the specification requires.
    std::vector<FlexItem> items;
    items.reserve(box->children().size());

    for (const auto &childPtr : box->children()) {
        Box *child = childPtr.get();
        const css::ComputedStyle *style = child->style();
        if (!style || !style->generatesBox())
            continue;

        // An absolutely positioned child is out of flow and is not a flex item.
        if (style->position == QLatin1String("absolute")
            || style->position == QLatin1String("fixed"))
            continue;

        FlexItem item;
        item.box = child;
        item.style = style;
        item.grow = style->flexGrow;
        item.shrink = style->flexShrink;
        item.alignSelf = resolveAlignSelf(item, containerStyle);
        item.order = style->order;
        items.push_back(item);
    }

    if (items.empty())
        return 0;

    // A stable sort keeps equal orders in document order, which the
    // specification requires. It is skipped when nothing sets an order, since
    // that is the common case.
    bool needsOrdering = false;
    for (const FlexItem &item : items) {
        if (item.order != 0) {
            needsOrdering = true;
            break;
        }
    }
    if (needsOrdering) {
        std::stable_sort(items.begin(), items.end(),
                         [](const FlexItem &a, const FlexItem &b) { return a.order < b.order; });
    }

    // ------------------------------------------------------ step 1: base size
    //
    // Each item's base size is its flex basis, or its own size when the basis is
    // auto. A definite basis wins over the item's width; a percentage basis
    // resolves against the container's main size.
    for (FlexItem &item : items) {
        const css::ComputedStyle *style = item.style;

        // Margins are read before the basis, because the base size is the
        // content-box size and the margins are added around it separately.
        if (rowDirection) {
            const FlexMargin marginLeft = {style->marginLeft.isAuto()
                                               ? 0
                                               : style->marginLeft.resolve(context.availableWidth),
                                           style->marginLeft.isAuto()};
            const FlexMargin marginRight = {style->marginRight.isAuto()
                                                ? 0
                                                : style->marginRight.resolve(context.availableWidth),
                                            style->marginRight.isAuto()};
            item.marginMainBefore = marginLeft.value;
            item.marginMainAfter = marginRight.value;
        } else {
            const double base = context.availableWidth;
            const FlexMargin marginTop = {style->marginTop.isAuto()
                                              ? 0
                                              : style->marginTop.resolve(base),
                                          style->marginTop.isAuto()};
            const FlexMargin marginBottom
                = {style->marginBottom.isAuto() ? 0 : style->marginBottom.resolve(base),
                   style->marginBottom.isAuto()};
            item.marginMainBefore = marginTop.value;
            item.marginMainAfter = marginBottom.value;
        }

        const Edges padding = paddingOf(item.box, context.availableWidth);
        const Edges border = borderOf(item.box);
        item.mainPaddingBorder = rowDirection ? padding.left + padding.right + border.left
                + border.right
                                              : padding.top + padding.bottom + border.top
                + border.bottom;
        item.crossPaddingBorder = rowDirection ? padding.top + padding.bottom + border.top
                + border.bottom
                                               : padding.left + padding.right + border.left
                + border.right;

        // The basis: an explicit one, else the item's own size, else its content.
        double baseSize = 0;
        bool haveBase = false;

        if (style->hasFlexBasis) {
            const double basisBase = rowDirection ? context.availableWidth
                                                  : (context.availableHeight >= 0
                                                         ? context.availableHeight
                                                         : context.availableWidth);
            baseSize = style->flexBasis.resolve(basisBase);
            haveBase = true;
        }

        if (!haveBase) {
            const css::LengthOrAuto &ownSize = rowDirection ? style->width : style->height;
            if (!ownSize.isAuto()) {
                baseSize = ownSize.resolve(rowDirection ? context.availableWidth
                                                        : (context.availableHeight >= 0
                                                               ? context.availableHeight
                                                               : context.availableWidth));
                haveBase = true;
            } else if (style->maxWidth.isLength() && rowDirection) {
                // A max-width caps the base size, which matters for the common
                // `flex: 0 1 auto; max-width: 100%` card pattern.
                baseSize = qMin(baseSize,
                                style->maxWidth.resolve(context.availableWidth));
            }
        }

        if (!haveBase) {
            // The basis is the item's content size. It is measured by laying the
            // item out with no constraint, which is the only way to know how
            // wide a row of text is. The measurement is thrown away; the real
            // layout happens once the final size is known.
            const double contentMain = rowDirection
                ? shrinkToFitWidth(item.box, context)
                : [&] {
                      Context measuring = context;
                      measuring.depth = context.depth + 1;
                      measuring.contentX = 0;
                      measuring.contentY = 0;
                      measuring.availableWidth = context.availableWidth;
                      measuring.availableHeight = -1;
                      const double savedDepth = 0;
                      Q_UNUSED(savedDepth);
                      layoutBlock(item.box, measuring, 0);
                      return item.box->borderBox().height() - item.crossPaddingBorder;
                  }();
            baseSize = contentMain;
        }

        // The base size is a content-box size: the padding and border are kept
        // out of it and added back when the outer size is needed. A border-box
        // basis or width already includes them, so they come off here, which is
        // what stops a padded card from overflowing the row it is flexible in.
        if (style->boxSizing == QLatin1String("border-box"))
            baseSize -= item.mainPaddingBorder;

        item.baseSize = qMax(0.0, baseSize);
        item.mainSize = item.baseSize;
    }

    // ------------------------------------------------- step 2: lines
    //
    // With wrapping, items are distributed into lines so that each line fits the
    // available main size. The hypothesis is the item's base size plus its
    // outer furniture; an item that alone exceeds the container still starts a
    // line, which is what browsers do rather than dropping it.
    struct Line
    {
        std::vector<FlexItem *> items;
        double mainSize = 0;
        double crossSize = 0;
        /// The total outer main size the items ask for before growing or
        /// shrinking, used to compute the free space.
        double outerMainSize = 0;
    };

    std::vector<Line> lines;

    if (!wrapsLines(containerStyle)) {
        Line line;
        for (FlexItem &item : items)
            line.items.push_back(&item);
        lines.push_back(line);
    } else {
        Line current;
        for (FlexItem &item : items) {
            const double outer = item.baseSize + item.mainPaddingBorder + item.marginMainBefore
                + item.marginMainAfter;

            // The gap belongs to the line only when it already has an item.
            const double needed = outer + (current.items.empty() ? 0.0 : gapMain);

            if (!current.items.empty() && mainAvailable >= 0
                && current.outerMainSize + needed > mainAvailable) {
                lines.push_back(current);
                current = Line();
            }

            if (!current.items.empty())
                current.outerMainSize += gapMain;
            current.outerMainSize += outer;
            current.items.push_back(&item);
        }
        if (!current.items.empty())
            lines.push_back(current);
    }

    // ------------------------------------------ step 3: resolve flexible lengths
    //
    // On each line the free space is distributed by the grow factors, or the
    // deficit taken back by the shrink factors. Growing uses the raw factor;
    // shrinking weights by the factor times the base size, so a large item gives
    // up more than a small one, which is what stops small items vanishing.
    for (Line &line : lines) {
        const int count = static_cast<int>(line.items.size());
        double totalOuter = 0;
        double totalGap = gapMain * qMax(0, count - 1);

        for (FlexItem *item : line.items) {
            totalOuter += item->baseSize + item->mainPaddingBorder + item->marginMainBefore
                + item->marginMainAfter;
        }

        const double availableForLine = mainAvailable >= 0 ? mainAvailable : totalOuter;
        double freeSpace = availableForLine - totalOuter - totalGap;

        // Auto margins on the main axis absorb free space before grow does,
        // which is how `margin-left: auto` pushes an item to the right.
        double autoMarginCount = 0;
        for (FlexItem *item : line.items) {
            if (rowDirection) {
                if (item->style->marginLeft.isAuto())
                    ++autoMarginCount;
                if (item->style->marginRight.isAuto())
                    ++autoMarginCount;
            } else {
                if (item->style->marginTop.isAuto())
                    ++autoMarginCount;
                if (item->style->marginBottom.isAuto())
                    ++autoMarginCount;
            }
        }

        if (autoMarginCount > 0 && freeSpace > 0) {
            const double each = freeSpace / autoMarginCount;
            for (FlexItem *item : line.items) {
                if (rowDirection) {
                    if (item->style->marginLeft.isAuto())
                        item->marginMainBefore = each;
                    if (item->style->marginRight.isAuto())
                        item->marginMainAfter = each;
                } else {
                    if (item->style->marginTop.isAuto())
                        item->marginMainBefore = each;
                    if (item->style->marginBottom.isAuto())
                        item->marginMainAfter = each;
                }
            }
            freeSpace = 0;
        }

        if (freeSpace > 0) {
            double totalGrow = 0;
            for (FlexItem *item : line.items)
                totalGrow += item->grow;

            if (totalGrow > 0) {
                for (FlexItem *item : line.items) {
                    if (item->grow <= 0)
                        continue;
                    item->mainSize = item->baseSize + freeSpace * (item->grow / totalGrow);
                }
            }
        } else if (freeSpace < 0) {
            // Shrinking is weighted, and clamped so an item never goes below
            // zero. A single pass is enough here because the weights are stable;
            // a full implementation would iterate to convergence.
            double totalWeight = 0;
            for (FlexItem *item : line.items)
                totalWeight += item->shrink * item->baseSize;

            if (totalWeight > 0) {
                for (FlexItem *item : line.items) {
                    if (item->shrink <= 0)
                        continue;
                    const double share = (-freeSpace) * ((item->shrink * item->baseSize) / totalWeight);
                    item->mainSize = qMax(0.0, item->baseSize - share);
                }
            } else {
                // Nothing may shrink, so the content overflows, which is what a
                // browser does with `flex-shrink: 0`.
                for (FlexItem *item : line.items)
                    item->mainSize = item->baseSize;
            }
        } else {
            for (FlexItem *item : line.items)
                item->mainSize = item->baseSize;
        }

        // A min-width or max-width still applies after flexing, which is what
        // stops a shrunk item collapsing past its floor.
        for (FlexItem *item : line.items) {
            if (rowDirection) {
                if (!item->style->minWidth.isAuto())
                    item->mainSize = qMax(item->mainSize,
                                          item->style->minWidth.resolve(context.availableWidth));
                if (!item->style->maxWidth.isAuto())
                    item->mainSize = qMin(item->mainSize,
                                          item->style->maxWidth.resolve(context.availableWidth));
            } else {
                if (!item->style->minHeight.isAuto())
                    item->mainSize = qMax(item->mainSize,
                                          item->style->minHeight.resolve(context.availableWidth));
                if (!item->style->maxHeight.isAuto())
                    item->mainSize = qMin(item->mainSize,
                                          item->style->maxHeight.resolve(context.availableWidth));
            }
        }
    }

    // -------------------------------------------- step 4: place and measure
    //
    // Each item is laid out at its final main size, which is what determines its
    // cross size when the cross size is not definite.
    const bool crossIsDefinite = crossAvailable >= 0;

    for (Line &line : lines) {
        for (FlexItem *item : line.items) {
            const double crossConstraint
                = crossIsDefinite
                    ? qMax(0.0, crossAvailable - item->crossPaddingBorder
                                    - item->marginCrossBefore - item->marginCrossAfter)
                    : -1;

            Context itemContext = context;
            itemContext.depth = context.depth + 1;

            // A row item is laid out with its resolved main size as the width,
            // so its inline content wraps at the right place. A column item's
            // width is the container's, and its height is the resolved size.
            if (rowDirection) {
                itemContext.availableWidth = item->mainSize;
                itemContext.availableHeight = crossConstraint;
            } else {
                itemContext.availableWidth = crossConstraint >= 0 ? crossConstraint
                                                                  : context.availableWidth;
                itemContext.availableHeight = item->mainSize;
            }

            itemContext.contentX = 0;
            itemContext.contentY = 0;

            // Flex has already decided the item's main size, so the ordinary
            // width and height resolution must not decide it again: an item with
            // `width: 10px; flex: 0 0 150px` is 150px wide, because the basis
            // wins. The properties are overridden for the duration of this
            // layout and restored afterwards, which is cheaper than threading a
            // "use this size instead" parameter through the whole layout engine.
            const css::ComputedStyle *original = item->box->style();
            css::ComputedStyle sized = *original;

            // The overrides below are content sizes, because that is what
            // layoutBlock works in, so the sizing mode is switched to
            // content-box for them. Otherwise a border-box item would have its
            // padding subtracted a second time and come out short.
            sized.boxSizing = QStringLiteral("content-box");

            if (item->mainSize >= 0) {
                if (rowDirection)
                    sized.width = css::LengthOrAuto::pixels(item->mainSize);
                else
                    sized.height = css::LengthOrAuto::pixels(item->mainSize);
            }
            if (crossConstraint >= 0) {
                // Only when align-self will not stretch the item: a stretch is
                // applied by the later pass, which needs the item's own height.
                const bool stretching = item->alignSelf == QLatin1String("stretch");
                if (!stretching) {
                    if (rowDirection)
                        sized.height = css::LengthOrAuto::pixels(crossConstraint);
                    else
                        sized.width = css::LengthOrAuto::pixels(crossConstraint);
                }
            }
            item->box->setStyle(&sized);

            layoutBlock(item->box, itemContext, 0);

            item->box->setStyle(original);

            // The item's cross size is what it came out as, unless a definite
            // cross size was imposed.
            const double laidOutCross = rowDirection
                ? item->box->borderBox().height() - item->crossPaddingBorder
                : item->box->borderBox().width() - item->crossPaddingBorder;

            item->crossSize = crossConstraint >= 0 ? crossConstraint : laidOutCross;

        }
    }

    // ---------------------------------------- step 5: cross sizes and lines
    for (Line &line : lines) {
        double maxCross = 0;
        for (FlexItem *item : line.items) {
            maxCross = qMax(maxCross,
                            item->crossSize + item->crossPaddingBorder
                                + item->marginCrossBefore + item->marginCrossAfter);
        }
        line.crossSize = crossIsDefinite ? qMax(maxCross, qMin(crossAvailable, maxCross))
                                        : maxCross;

    }

    double totalCross = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        totalCross += lines.at(i).crossSize;
        if (i + 1 < lines.size())
            totalCross += gapCross;
    }

    // align-content positions the whole set of lines when they do not fill the
    // container, which only has an effect once there is more than one line.
    double linesOffset = 0;
    double lineSpacing = 0;

    if (crossIsDefinite && lines.size() > 1) {
        const double leftover = crossAvailable - totalCross;
        if (leftover > 0) {
            const QString &align = containerStyle->alignContent;
            if (align == QLatin1String("flex-end"))
                linesOffset = leftover;
            else if (align == QLatin1String("center"))
                linesOffset = leftover / 2;
            else if (align == QLatin1String("space-between"))
                lineSpacing = leftover / static_cast<double>(lines.size() - 1);
            else if (align == QLatin1String("space-around"))
                lineSpacing = leftover / static_cast<double>(lines.size());
        }
    }

    // ------------------------------------------- step 6: place everything
    double crossCursor = linesOffset;

    // The cross axis runs top to bottom for a row and left to right for a
    // column; wrap-reverse flips it.
    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        Line &line = lines.at(lineIndex);

        // Distribute the main-axis free space that justify-content owns.
        double mainOffset = 0;
        double spacing = 0;

        const double lineOuter = [&] {
            double total = 0;
            for (FlexItem *item : line.items) {
                total += item->mainSize + item->mainPaddingBorder + item->marginMainBefore
                    + item->marginMainAfter;
            }
            return total;
        }();

        const double lineGapTotal = gapMain * qMax(0, static_cast<int>(line.items.size()) - 1);
        const double mainLeftover
            = mainAvailable >= 0 ? mainAvailable - lineOuter - lineGapTotal : 0;

        if (mainLeftover > 0) {
            const QString &justify = containerStyle->justifyContent;
            if (justify == QLatin1String("flex-end"))
                mainOffset = mainLeftover;
            else if (justify == QLatin1String("center"))
                mainOffset = mainLeftover / 2;
            else if (justify == QLatin1String("space-between") && line.items.size() > 1)
                spacing = mainLeftover / static_cast<double>(line.items.size() - 1);
            else if (justify == QLatin1String("space-around") && !line.items.empty()) {
                spacing = mainLeftover / static_cast<double>(line.items.size());
                mainOffset = spacing / 2;
            } else if (justify == QLatin1String("space-evenly") && !line.items.empty()) {
                spacing = mainLeftover / static_cast<double>(line.items.size() + 1);
                mainOffset = spacing;
            }
        }

        // A reversed container starts from the far end, so the first item is
        // placed last.
        // A reversed container starts from the far end of the line, so the first
        // item in the document is placed last and therefore right-most. The
        // starting point is the line's own extent, not the leftover: a line that
        // exactly fills the container has no leftover, yet the items still start
        // from the right.
        const double lineExtent = mainAvailable >= 0 ? mainAvailable
                                                     : mainOffset + lineOuter + lineGapTotal;
        double mainCursor = mainOffset;
        if (reverse)
            mainCursor = lineExtent;

        for (FlexItem *item : line.items) {
            const double outerMain = item->mainSize + item->mainPaddingBorder
                + item->marginMainBefore + item->marginMainAfter;

            if (reverse)
                mainCursor -= outerMain + item->marginMainBefore;

            const double itemMainStart = mainCursor + item->marginMainBefore;

            // The cross-axis position from align-self.
            const double outerCross = item->crossSize + item->crossPaddingBorder
                + item->marginCrossBefore + item->marginCrossAfter;
            double crossStart = crossCursor;

            if (item->alignSelf == QLatin1String("flex-end")
                || (item->alignSelf == QLatin1String("end"))) {
                crossStart += line.crossSize - outerCross;
            } else if (item->alignSelf == QLatin1String("center")) {
                crossStart += (line.crossSize - outerCross) / 2;
            } else if (item->alignSelf == QLatin1String("stretch")) {
                // Stretch makes the item fill the line on the cross axis, unless
                // its own size is already definite. A row item's height and a
                // column item's width are the ones that stretch.
                const bool ownSizeDefinite = rowDirection ? !item->style->height.isAuto()
                                                          : !item->style->width.isAuto();
                if (!ownSizeDefinite) {
                    const double stretchTo
                        = qMax(0.0, line.crossSize - item->crossPaddingBorder
                                        - item->marginCrossBefore - item->marginCrossAfter);
                    if (stretchTo > item->crossSize) {
                        item->crossSize = stretchTo;

                        Context stretchContext = context;
                        stretchContext.depth = context.depth + 1;
                        stretchContext.contentX = 0;
                        stretchContext.contentY = 0;
                        if (rowDirection) {
                            stretchContext.availableWidth = item->mainSize;
                            stretchContext.availableHeight = item->crossSize;
                        } else {
                            stretchContext.availableWidth = item->crossSize;
                            stretchContext.availableHeight = item->mainSize;
                        }

                        // The stretch is applied as a definite size, because an
                        // auto height would simply be re-measured from the
                        // content and come back as the unstretched value.
                        const css::ComputedStyle *unstretched = item->box->style();
                        css::ComputedStyle stretched = *unstretched;
                        stretched.boxSizing = QStringLiteral("content-box");
                        if (rowDirection)
                            stretched.height = css::LengthOrAuto::pixels(item->crossSize);
                        else
                            stretched.width = css::LengthOrAuto::pixels(item->crossSize);
                        item->box->setStyle(&stretched);

                        layoutBlock(item->box, stretchContext, 0);

                        item->box->setStyle(unstretched);
                    }
                }
            }

            crossStart += item->marginCrossBefore;

            // The item's own padding and border sit outside its content box, and
            // layoutBlock places the border box, so the origin is the item's
            // border-box corner.
            // Recorded so a column container can report its height without
            // another walk over the items.
            item->mainPosition = itemMainStart;
            item->crossPosition = crossStart;

            const double borderX = rowDirection ? itemMainStart : crossStart;
            const double borderY = rowDirection ? crossStart : itemMainStart;

            placeBoxAt(item->box, context.contentX + borderX, context.contentY + borderY);

            if (reverse)
                mainCursor -= item->marginMainAfter + spacing;
            else
                mainCursor += outerMain + spacing + gapMain;
        }

        crossCursor += line.crossSize + gapCross + lineSpacing;
    }

    // ------------------------------------------------------ container height
    //
    // The cross axis contributes when it is vertical, and the main axis when it
    // is: a row's height is the total of its lines, a column's is the total of
    // its items. The larger of the two is what the caller needs, because either
    // can be the one that determines how tall the container is.
    const double totalCrossWithGaps
        = totalCross + (lines.size() > 1 ? gapCross * static_cast<double>(lines.size() - 1) : 0);

    if (rowDirection)
        return qMax(0.0, totalCrossWithGaps);

    // For a column the items were placed with their resolved heights, so the
    // furthest bottom edge is the content height.
    double tallest = 0;
    for (const Line &line : lines) {
        double lineCursor = line.crossSize;
        for (FlexItem *item : line.items) {
            const double bottom = item->mainPosition + item->mainSize + item->mainPaddingBorder
                + item->marginMainAfter;
            tallest = qMax(tallest, bottom);
        }
        Q_UNUSED(lineCursor);
    }

    return qMax(tallest, totalCrossWithGaps);
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

    // A definite height is passed down, because a child can need it: a column
    // flex container distributes its items along the vertical axis, and a
    // percentage height on a child resolves against its parent. Reporting -1
    // here made both impossible, which is why `justify-content` had no room to
    // work with in a column.
    const double boxSpecifiedHeight = resolveUsedHeight(box, context);
    inner.availableHeight = boxSpecifiedHeight >= 0
        ? qMax(0.0, boxSpecifiedHeight - padding.top - padding.bottom - border.top
                        - border.bottom)
        : -1;

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

        // A flex container lays its children out along an axis instead of
        // stacking them, so it takes neither the block nor the inline path.
        if (style->isFlexContainer())
            contentHeight = layoutFlexChildren(box, inner);
        else if (hasBlockChild)
            contentHeight = layoutBlockChildren(box, inner, &trailingMargin);
        else
            contentHeight = layoutInlineRun(box, inner);
    }

    // A specified height wins over the content height (CSS 2.2 §10.6.3).
    // resolveUsedHeight() already reports a content height: it subtracts the
    // padding and border itself for a border-box, and a content-box height is
    // the content height by definition. Subtracting them again here would make a
    // border-box element short by its own padding.
    const double specifiedHeight = boxSpecifiedHeight;
    if (specifiedHeight >= 0)
        contentHeight = qMax(0.0, specifiedHeight);

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
