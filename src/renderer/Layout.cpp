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

LayoutEngine::TextMetric LayoutEngine::measure(const QFont &font, const QString &text) const
{
    // The key carries everything that changes the shaping result. It is built as
    // one string rather than a nested map so that a lookup is a single hash: the
    // hot path here is millions of calls, and a level of indirection per call
    // would cost more than the longer key does.
    static QString key;
    key.clear();
    key += QString::number(font.pointSizeF(), 'f', 3);
    key += u'\x1f';
    key += QString::number(static_cast<int>(font.weight()));
    key += font.italic() ? u'I' : u'R';
    key += u'\x1f';
    key += font.families().join(u',');
    key += u'\x1f';
    key += text;

    const auto found = m_textMetrics.constFind(key);
    if (found != m_textMetrics.constEnd())
        return found.value();

    const QFontMetricsF metrics(font);
    TextMetric measured;
    measured.width = metrics.horizontalAdvance(text);
    measured.ascent = metrics.ascent();
    measured.descent = metrics.descent();
    measured.lineHeight = metrics.height();

    // The cache is bounded, because a page that generates a unique string per
    // element - ids, timestamps, hashes - would otherwise grow it without limit.
    // Clearing wholesale rather than evicting one entry keeps the common case a
    // single hash lookup; the cost is one pass of re-measuring after the limit,
    // which is bounded and rare.
    static constexpr int kMaxEntries = 200000;
    if (m_textMetrics.size() >= kMaxEntries)
        m_textMetrics.clear();

    const auto inserted = m_textMetrics.insert(key, measured);
    return inserted.value();
}

double LayoutEngine::measureCharacter(const QFont &font, QChar character) const
{
    static QString key;
    key.clear();
    key += QString::number(font.pointSizeF(), 'f', 3);
    key += u'\x1f';
    key += QString::number(static_cast<int>(font.weight()));
    key += font.italic() ? u'I' : u'R';
    key += u'\x1f';
    key += font.families().join(u',');
    key += u'\x1f';
    key += character;

    const auto found = m_characterWidths.constFind(key);
    if (found != m_characterWidths.constEnd())
        return found.value();

    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(character);
    m_characterWidths.insert(key, width);
    return width;
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

    std::function<void(const Box *, double)> walkContent = [&](const Box *node, double indent) {
        if (!node)
            return;

        // An out-of-flow child takes no space, so it contributes nothing to a
        // shrink-to-fit width. Without this a positioned dropdown would widen the
        // menu it hangs from, which is the opposite of what absolute positioning
        // is for.
        if (node != box && node->isOutOfFlow())
            return;

        if (node->isText()) {
            const css::ComputedStyle *style = node->style() ? node->style() : box->style();
            preferred = qMax(preferred, indent + measure(fontFor(style), node->text()).width);
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
            walkContent(child.get(), ownIndent);
    };

    for (const auto &child : box->children())
        walkContent(child.get(), 0);

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
            item->box->setStyle(arenaStyle(sized));

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
                        item->box->setStyle(arenaStyle(stretched));

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

        const double childMarginTop = childStyle->marginTop.isAuto()
            ? 0
            : childStyle->marginTop.resolve(context.availableWidth);

        // An absolutely positioned child is out of flow: it is placed against
        // its containing block after everything else has been laid out, and
        // takes no space here. Skipping it keeps it from shifting its siblings,
        // and keeps its margins out of the collapsing calculation.
        if (child->isOutOfFlow())
            continue;

        // A float is taken out of flow but still pushes the content that follows
        // it aside. It is placed here, at the current y, and contributes no
        // height to the flow - which is exactly why a parent with only floated
        // children collapses unless it establishes a formatting context.
        if (childStyle->isFloating() && !child->isText()) {
            // The float is placed at the flow position, before the collapsed
            // margin, because its own top margin is inside the band it registers.
            placeFloat(child, context, y + qMax(collapsedMargin, childMarginTop));
            continue;
        }

        // `clear` moves the box below the floats it names, and it also ends the
        // margin collapsing: the two boxes are no longer adjacent, so their
        // margins must not merge.
        if (childStyle->clear != QLatin1String("none") && context.floats) {
            const double cleared = context.floats->clearY(y + qMax(collapsedMargin, childMarginTop),
                                                         childStyle->clear);
            if (cleared > y + qMax(collapsedMargin, childMarginTop)) {
                y = cleared;
                collapsedMargin = 0;
            }
        }

        // Vertical margins between adjacent siblings collapse: the gap between
        // two boxes is the larger of the two margins, never their sum
        // (CSS 2.2 §8.3.1). The gap is therefore computed here, and the child is
        // laid out with no margin added on top of it.
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

    // Floats found among the inline content. The collection pass takes them out
    // of the line, and they are placed once the lines are about to be built, so
    // a line beside a floated image starts where the image ends.
    std::vector<Box *> floatedPointers;

    std::function<void(Box *, const css::ComputedStyle *)> collect
        = [&](Box *box, const css::ComputedStyle *inherited) {
              const css::ComputedStyle *style = box->style() ? box->style() : inherited;
              if (!style)
                  return;

              // A float is out of flow even when its element is inline-level,
              // which is the usual case for a floated image or a floated span.
              // Collecting it as a fragment would dissolve it into the line and
              // let the text flow through it, so it is taken out here and pushed
              // aside onto the float list instead.
              //
              // Only an inline-level float is handled here. A block-level float
              // was already placed by layoutBlockChildren() - one of the two
              // passes runs, never both - so collecting it here would place it a
              // second time and shift it by its own width.
              if (style->isFloating() && !box->isText() && context.floats
                  && !box->isBlockLevel()) {
                  floatedPointers.push_back(box);
                  return;
              }

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
                  // Measured once for the whole text box: the ascent and the
                  // height are the font's, and the string only matters for the
                  // cached case, so one entry serves every word in it.
                  const TextMetric metrics = measure(font, box->text());
                  const double lineHeight
                      = style->lineHeight > 0 ? style->lineHeight : metrics.lineHeight;

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
                      fragment.width = measure(font, word).width;
                      fragment.height = lineHeight;
                      fragment.baseline = metrics.ascent;
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

              for (const auto &child : box->children()) {
                  // An out-of-flow child takes no space in the line and is laid
                  // out separately, so neither it nor its content is measured
                  // here. Measuring it made a positioned dropdown's text add a
                  // line to the box it hangs from.
                  if (child->isOutOfFlow())
                      continue;
                  collect(child.get(), style);
              }
          };

    collect(container, container->style());

    // The inline children have now been measured into fragments, which carry the
    // text, the resolved style and the geometry. Keeping them around would leave
    // unpositioned text boxes at the container's origin for the painter to draw,
    // so they are released below. Replaced boxes are the exception: they hold
    // real content and are re-attached afterwards, in place.
    // The children that have to survive the rebuild below: replaced and
    // inline-block boxes, which hold real content the lines only reference by
    // index, and absolutely positioned boxes, which never took part in a line at
    // all and are laid out separately once the whole tree has its geometry.
    std::vector<Box *> replacedPointers;
    std::vector<Box *> outOfFlowPointers;

    std::function<void(Box *)> findReplaced = [&](Box *parent) {
        for (const auto &child : parent->children()) {
            // An out-of-flow child contributes nothing to its parent's line, and
            // is kept aside so the rebuild below does not destroy it.
            if (child->isOutOfFlow()) {
                outOfFlowPointers.push_back(child.get());
                continue;
            }

            // A float is collected by the measurement pass instead, since it is
            // not atomic to a line in the way a replaced box is.
            if (child->style() && child->style()->isFloating())
                continue;

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

    // An out-of-flow box never took part in a line, so it is detached and put
    // back rather than being destroyed by the rebuild below. Without this an
    // absolutely positioned box inside a container with inline content lost its
    // own subtree and was left with no height.
    std::vector<std::unique_ptr<Box>> outOfFlowBoxes;
    for (Box *box : outOfFlowPointers) {
        if (Box *parent = box->parent()) {
            if (std::unique_ptr<Box> owned = parent->detachChild(box))
                outOfFlowBoxes.push_back(std::move(owned));
        }
    }

    // A floated inline element is detached for the same reason: the rebuild
    // below destroys every child, and a float holds a real subtree.
    //
    // Each is put back on the container it came from - recorded before the
    // detach, because detaching clears the parent pointer. Re-appending to the
    // wrong box is not a subtle bug: a float that became its own child made
    // layout recurse into itself until the stack ran out.
    std::vector<std::unique_ptr<Box>> floatedBoxes;
    std::vector<Box *> floatedParents;
    for (Box *box : floatedPointers) {
        if (Box *parent = box->parent()) {
            if (std::unique_ptr<Box> owned = parent->detachChild(box)) {
                floatedParents.push_back(parent);
                floatedBoxes.push_back(std::move(owned));
            }
        }
    }

    container->clearChildren();

    for (auto &owned : outOfFlowBoxes)
        container->appendChild(std::move(owned));

    std::vector<Box *> restoredFloats;
    for (size_t i = 0; i < floatedBoxes.size(); ++i) {
        restoredFloats.push_back(floatedBoxes[i].get());
        floatedParents[i]->appendChild(std::move(floatedBoxes[i]));
    }

    // ------------------------------------------------------------ line building

    // A floated inline element is placed before the lines are built, because the
    // first line has to know it is there: a text line beside a floated image
    // starts where the image ends. A float from the block pass was already placed
    // by layoutBlockChildren(), so these are only the ones the collection pass
    // found among the inline content.
    //
    // The box is already a child of this container - the collection pass only
    // recorded a pointer to it - so placing it here registers its band and gives
    // it geometry without moving it in the tree.
    for (Box *floatBox : restoredFloats)
        placeFloat(floatBox, context, context.contentY);

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

        // A line sits in whatever room the floats leave at its own y, so its
        // origin and width come from the span rather than from the container.
        const AvailableSpan lineSpan = spanAt(context, y);

        auto lineOwner = std::make_unique<Box>(Box::Type::Line, nullptr, container->style());
        Box *line = container->appendChild(std::move(lineOwner));

        // The text fragments on this line are attached to it with absolute
        // geometry, so the painter can draw them directly.
        double x = lineSpan.x;
        for (int i = lineStartFragment; i < endIndex && i < fragments.size(); ++i) {
            const Fragment &fragment = fragments.at(i);
            if (fragment.breakBefore)
                continue;

            const double spaceWidth = fragment.spaceBefore
                ? measureCharacter(fontFor(fragment.style), u' ')
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

        line->setPosition(lineSpan.x, y);
        // The line box itself is only as wide as its content, which is what the
        // painter and hit testing expect; the container's width is a layout
        // concern, not a property of the line.
        const double usedWidth = qMin(lineWidth, lineSpan.width);
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

        const double spaceWidth
            = fragment.spaceBefore ? measureCharacter(fontFor(fragment.style), u' ') : 0.0;

        // A float narrows the line it overlaps, so the width a line is broken
        // against is not the container's but whatever room is left beside the
        // floats. It can also shift the line's start, which is what makes text
        // sit to the right of a left float.
        const AvailableSpan span = spanAt(context, y);
        if (span.width < lineWidth + spaceWidth + fragment.width && lineWidth == 0
            && context.floats) {
            // Nothing fits on this line at all beside the float, so the line
            // starts below it rather than breaking into zero-width lines.
            const double next = context.floats->nextBandY(y);
            if (next > y)
                y = next;
        }

        const AvailableSpan lineSpan = spanAt(context, y);
        const bool mustWrap
            = lineWidth > 0 && (lineWidth + spaceWidth + fragment.width) > lineSpan.width;

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

// ------------------------------------------------------------------- floats
//
// A float is taken out of normal flow but still affects it: the box that
// follows flows *beside* it rather than through it. That is the whole of the
// model, and it is why floats cannot be implemented by simply skipping the box.
//
// The geometry is tracked as bands rather than as a list of boxes, because what
// the flow needs to know is only "how much room is there at this y". A band is
// the float's margin box, since its margins are as much an obstacle as its
// border box.

void LayoutEngine::FloatContext::spanAt(double y, double contentX, double contentWidth,
                                        double *leftInset, double *rightLimit) const
{
    *leftInset = contentX;
    *rightLimit = contentX + contentWidth;

    for (const FloatBand &band : bands) {
        // Only a float that overlaps this y constrains it. A float that ends
        // above leaves the line untouched, which is what lets text resume at
        // full width below a floated image.
        if (y < band.top || y >= band.bottom)
            continue;

        if (band.isLeft)
            *leftInset = qMax(*leftInset, band.right);
        else
            *rightLimit = qMin(*rightLimit, band.left);
    }

    // Floats may between them leave nothing, in which case the span collapses to
    // nothing rather than inverting.
    if (*rightLimit < *leftInset)
        *rightLimit = *leftInset;
}

double LayoutEngine::FloatContext::nextBandY(double y) const
{
    // The lowest bottom among floats that reach this y. Not the lowest overall:
    // a float far below, which a clear would move to, must not drag ordinary
    // content down with it.
    double lowest = y;
    for (const FloatBand &band : bands) {
        if (y >= band.top && y < band.bottom)
            lowest = qMax(lowest, band.bottom);
    }
    return lowest;
}

double LayoutEngine::FloatContext::clearY(double y, const QString &clear) const
{
    if (clear.isEmpty() || clear == QLatin1String("none"))
        return y;

    const bool wantLeft = clear == QLatin1String("left") || clear == QLatin1String("both");
    const bool wantRight = clear == QLatin1String("right") || clear == QLatin1String("both");

    double lowest = y;
    for (const FloatBand &band : bands) {
        // Only floats below the current position matter: clear moves a box down
        // past them, and one already above it is already cleared.
        if (band.bottom <= y)
            continue;
        if ((band.isLeft && wantLeft) || (!band.isLeft && wantRight))
            lowest = qMax(lowest, band.bottom);
    }
    return lowest;
}

double LayoutEngine::FloatContext::bottom() const
{
    double lowest = 0;
    for (const FloatBand &band : bands)
        lowest = qMax(lowest, band.bottom);
    return lowest;
}

LayoutEngine::AvailableSpan LayoutEngine::spanAt(const Context &context, double y) const
{
    // The room a block has is its own content box, whatever the formatting
    // context's edges are. Returning the flow's edges here would make a
    // `width: 200px` paragraph break its lines against the viewport.
    const double boxStart = context.contentX;
    const double boxEnd = context.contentX + context.availableWidth;

    if (!context.floats)
        return {boxStart, context.availableWidth};

    double leftInset = 0;
    double rightLimit = 0;
    context.floats->spanAt(y, context.flowX, context.flowWidth, &leftInset, &rightLimit);

    // A float narrows a line only where it reaches into the block's own content
    // box. Intersecting the two is what makes a float outside a nested block
    // leave that block alone while still pushing its siblings' text aside.
    const double start = qMax(boxStart, leftInset);
    const double end = qMin(boxEnd, rightLimit);
    return {start, qMax(0.0, end - start)};
}

void LayoutEngine::placeFloat(Box *box, const Context &context, double y)
{
    if (!context.floats) {
        // A float only has meaning inside a block formatting context. Without
        // one there is nothing to avoid it, so it is laid out in flow rather
        // than silently dropped.
        return;
    }

    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    const bool isLeft = style->floatSide != QLatin1String("right");

    // A float is positioned against its containing block's content box, not
    // against the formatting context's edges. The two differ whenever the float
    // sits inside a narrower box: a right float in a 300px child must land on
    // that child's right edge, not on the page's.
    const double containerStart = context.contentX;
    const double containerEnd = context.contentX + context.availableWidth;

    // A float never keeps an auto margin: there is no free space to absorb,
    // because the float shrink-to-fits.
    const auto marginOf = [&](const css::LengthOrAuto &length) {
        return length.isAuto() ? 0.0 : length.resolve(context.availableWidth);
    };
    const double marginLeft = marginOf(style->marginLeft);
    const double marginRight = marginOf(style->marginRight);
    const double marginTop = marginOf(style->marginTop);

    // The float itself is the obstacle, so it is laid out as though no float
    // were in force; otherwise it would avoid itself.
    Context floatContext = context;
    floatContext.floats = nullptr;

    // shrinkToFitWidth() returns an *outer* width: it measures the content and
    // adds the padding and border itself. It is the used width only when the
    // float has no width of its own - an empty floated div has a shrink-to-fit
    // width of zero, so capping a specified width by it would collapse every
    // `width: 120px` float to nothing.
    //
    // resolveUsedWidth() returns a *content* width, so the padding and border
    // are added back here to leave both branches describing the same thing: the
    // width the float's border box will have.
    const Edges floatPadding = paddingOf(box, context.availableWidth);
    const Edges floatBorder = borderOf(box);
    const double desiredWidth = style->width.isAuto()
        ? shrinkToFitWidth(box, floatContext)
        : resolveUsedWidth(box, context.availableWidth, nullptr) + floatPadding.left
            + floatPadding.right + floatBorder.left + floatBorder.right;

    // It can never be wider than its containing block leaves once its own
    // margins are taken out.
    const double room = qMax(0.0, containerEnd - containerStart - marginLeft - marginRight);
    const double borderBoxWidth = qMax(0.0, qMin(desiredWidth, room));

    // The room beside the floats already in force, clipped to the containing
    // block: leaving the block's own box would let a float escape it.
    const auto roomAt = [&](double atY, double *start, double *limit) {
        double flowLeft = 0;
        double flowRight = 0;
        context.floats->spanAt(atY, context.flowX, context.flowWidth, &flowLeft, &flowRight);
        *start = qMax(containerStart, flowLeft);
        *limit = qMin(containerEnd, flowRight);
    };

    double start = 0;
    double limit = 0;

    // The float drops until it fits beside the floats already there. A previous
    // float that occupies the full width leaves no room at any y until its
    // bottom, which is what puts the second float below the first.
    double placedY = qMax(y, context.contentY);
    const double needed = borderBoxWidth + marginLeft + marginRight;
    for (int guard = 0; guard < 256; ++guard) {
        roomAt(placedY, &start, &limit);
        if (limit - start >= needed)
            break;
        const double next = context.floats->nextBandY(placedY);
        if (next <= placedY)
            break;
        placedY = next;
    }
    roomAt(placedY, &start, &limit);

    Context boxContext = context;
    // The float's own containing block for its content is its own content box,
    // which starts at the containing block's start and is as wide as itself less
    // its own margins, padding and border.
    boxContext.contentX = containerStart;
    boxContext.contentY = placedY + marginTop;
    boxContext.availableWidth = qMax(0.0, borderBoxWidth - floatPadding.left - floatPadding.right
                                              - floatBorder.left - floatBorder.right);
    boxContext.floats = nullptr;

    layoutBlock(box, boxContext, placedY + marginTop);
    // layoutBlock() resolved the width from the style a second time, so the
    // border box is set here to the width the float was measured at. Without
    // this a shrink-to-fit float whose content is wider than the room available
    // would paint outside the band registered for it.
    box->setSize(borderBoxWidth, box->height());

    const double x = isLeft ? start + marginLeft : limit - marginRight - borderBoxWidth;
    box->setPosition(x, placedY + marginTop);

    // The band is the float's margin box, because its margins are as much of an
    // obstacle as its border box is.
    FloatBand band;
    band.box = box;
    band.isLeft = isLeft;
    band.left = isLeft ? start : x - marginLeft;
    band.right = isLeft ? x + borderBoxWidth + marginRight : limit;
    band.top = placedY;
    band.bottom = placedY + box->height() + marginTop;
    context.floats->bands.push_back(band);
}

// ---------------------------------------------------------------------- grid
//
// Grid layout has three phases that cannot be interleaved:
//
//   1. Place the items, so it is known how many tracks the grid has. An item
//      can widen the grid beyond its template, which is why this comes first.
//   2. Size the tracks, which needs every item's measured contribution.
//   3. Lay each item out into the cell it occupies.
//
// Doing them in another order does not merely reorder work: sizing before
// placing leaves no column for an item at an implicit track, and placing before
// measuring sizes a track against content that has not been laid out.

double LayoutEngine::widestWordWidth(Box *box, const Context &context) const
{
    // The min-content width is the widest thing that cannot be broken. For text
    // that is the widest word; for everything else it is the box's own width.
    double widest = 0;
    bool measuredText = false;

    std::function<void(const Box *)> walk = [&](const Box *node) {
        if (!node)
            return;
        if (node->isText()) {
            const css::ComputedStyle *style = node->style() ? node->style() : box->style();
            const QFont font = fontFor(style);
            for (const QString &word : node->text().split(u' ', Qt::SkipEmptyParts)) {
                measuredText = true;
                const double width = measure(font, word).width;
                widest = qMax(widest, width);
            }
            return;
        }
        if (node->isReplaced() || node->type() == Box::Type::InlineBlock) {
            measuredText = true;
            widest = qMax(widest, node->borderBox().width());
            return;
        }
        for (const auto &child : node->children())
            walk(child.get());
    };
    walk(box);

    // A box with no text is only as narrow as its padding and border allow.
    const css::ComputedStyle *style = box->style();
    if (style) {
        widest += style->paddingLeft.resolve(context.availableWidth)
            + style->paddingRight.resolve(context.availableWidth) + style->borderLeftWidth
            + style->borderRightWidth;
    }
    Q_UNUSED(measuredText);
    return widest;
}

double LayoutEngine::layoutGridChildren(Box *box, const Context &context)
{
    const css::ComputedStyle *containerStyle = box->style();
    if (!containerStyle)
        return 0;

    const double columnGap = containerStyle->columnGap.isAuto()
        ? 0
        : containerStyle->columnGap.resolve(context.availableWidth);
    const double rowGap
        = containerStyle->rowGap.isAuto() ? 0 : containerStyle->rowGap.resolve(context.availableWidth);

    // ------------------------------------------------------------- templates
    //
    // The track lists are parsed here rather than in the cascade because a
    // percentage or a viewport unit needs the containing block, which only
    // exists now.
    css::GridTrackList columnTemplate;
    css::GridTrackList rowTemplate;
    if (!containerStyle->gridTemplateColumns.isEmpty()) {
        css::parseTrackList(containerStyle->gridTemplateColumns, containerStyle->fontSize, 16,
                            context.availableWidth, &columnTemplate);
        columnTemplate.gap = containerStyle->columnGap.resolve(context.availableWidth);
    }
    if (!containerStyle->gridTemplateRows.isEmpty()) {
        css::parseTrackList(containerStyle->gridTemplateRows, containerStyle->fontSize, 16,
                            context.availableWidth, &rowTemplate);
        rowTemplate.gap = containerStyle->rowGap.resolve(context.availableWidth);
    }

    // ------------------------------------------------ collect and place items
    std::vector<GridItem> items;
    for (const auto &childPtr : box->children()) {
        Box *child = childPtr.get();
        const css::ComputedStyle *childStyle = child->style();
        if (!childStyle || !childStyle->generatesBox())
            continue;
        // An out-of-flow child is positioned against its containing block after
        // normal flow, and a float has no meaning inside a grid.
        if (child->isOutOfFlow() || childStyle->isFloating())
            continue;

        GridItem item;
        item.box = child;
        item.style = childStyle;
        items.push_back(item);
    }

    if (items.empty())
        return 0;

    // `auto-fit` collapses the repetitions that nothing landed in, so it has to
    // know how many items there are before the template is expanded. The count is
    // only an upper bound on the columns used - an item may span - which is
    // enough: collapsing more repetitions than there are items would drop tracks
    // that are needed.
    columnTemplate.autoPlacementItemCount = static_cast<int>(items.size());
    rowTemplate.autoPlacementItemCount = static_cast<int>(items.size());

    // A negative line counts back from the end of the *explicit* grid, so the
    // line count is needed before a placement can be resolved. -1 is the last
    // line, which is one past the last track.
    //
    // The templates are expanded here, at a size actually available, which is the
    // only point at which `repeat(auto-fill, ...)` can be resolved: how many
    // repetitions fit is a function of the container's width.
    // The expansion seeds the template's own track list, so everything below -
    // sizing, implicit tracks, offsets - works from the tracks that actually
    // exist rather than needing to know about the repetition.
    if (columnTemplate.autoRepeat) {
        columnTemplate.tracks.clear();
        for (const css::GridTrackEntry &entry : columnTemplate.expand(
                 context.availableWidth, containerStyle->fontSize, 16)) {
            columnTemplate.tracks.append(entry.track);
        }
    }
    if (rowTemplate.autoRepeat) {
        rowTemplate.tracks.clear();
        for (const css::GridTrackEntry &entry : rowTemplate.expand(
                 context.availableWidth, containerStyle->fontSize, 16)) {
            rowTemplate.tracks.append(entry.track);
        }
    }

    const int explicitColumns = qMax(1, columnTemplate.size());
    const int explicitRows = qMax(1, rowTemplate.size());

    const auto resolveLine = [](int line, int explicitCount) {
        // A line of n refers to the n-th grid line; -n counts back from the end.
        // Both are converted to a zero-based track index.
        if (line > 0)
            return line - 1;
        if (line < 0)
            return explicitCount + line;
        return -1; // auto
    };

    const auto namedLine = [](const css::GridTrackList &list, const QString &name, int *line) {
        if (name.isEmpty() || !list.lineNames.contains(name))
            return false;
        const QList<int> &lines = list.lineNames.value(name);
        if (lines.isEmpty())
            return false;
        // The first line with that name, which is what `grid-column: left` means
        // when a name appears once.
        *line = lines.first();
        return true;
    };

    // Pass 1: resolve the placements that were given explicitly, and give an
    // item with two explicit lines the span between them. This runs before
    // auto-placement so the cursor knows which cells are taken.
    int columnCount = explicitColumns;
    int rowCount = explicitRows;

    for (GridItem &item : items) {
        const css::GridPlacement &columns = item.style->gridColumnStart;
        const css::GridPlacement &rows = item.style->gridRowStart;

        // Columns.
        int start = -1;
        int end = -1;
        if (int named = 0; namedLine(columnTemplate, columns.startName, &named))
            start = named - 1;
        else if (columns.startLine != 0)
            start = resolveLine(columns.startLine, explicitColumns);
        if (int named = 0; namedLine(columnTemplate, columns.endName, &named))
            end = named - 1;
        else if (columns.endLine != 0)
            end = resolveLine(columns.endLine, explicitColumns);

        if (start >= 0) {
            if (end >= 0)
                end = qMax(end, start + 1);
            else if (columns.endSpan > 0)
                end = start + columns.endSpan;
            else if (columns.startSpan > 0)
                end = start + columns.startSpan;
            else
                end = start + 1;
        } else if (end >= 0) {
            start = columns.startSpan > 0 ? end - columns.startSpan : end - 1;
            start = qMax(0, start);
        }

        item.columnStart = start;
        item.columnEnd = end;
        // A span written without a start line survives as a hint, because by the
        // time auto-placement runs both lines are the "auto" marker.
        if (start < 0)
            item.columnSpanHint = qMax(columns.startSpan, columns.endSpan);

        // Rows.
        int rowStartIndex = -1;
        int rowEndIndex = -1;
        if (int named = 0; namedLine(rowTemplate, rows.startName, &named))
            rowStartIndex = named - 1;
        else if (rows.startLine != 0)
            rowStartIndex = resolveLine(rows.startLine, explicitRows);
        if (int named = 0; namedLine(rowTemplate, rows.endName, &named))
            rowEndIndex = named - 1;
        else if (rows.endLine != 0)
            rowEndIndex = resolveLine(rows.endLine, explicitRows);

        if (rowStartIndex >= 0) {
            if (rowEndIndex >= 0)
                rowEndIndex = qMax(rowEndIndex, rowStartIndex + 1);
            else if (rows.endSpan > 0)
                rowEndIndex = rowStartIndex + rows.endSpan;
            else if (rows.startSpan > 0)
                rowEndIndex = rowStartIndex + rows.startSpan;
            else
                rowEndIndex = rowStartIndex + 1;
        } else if (rowEndIndex >= 0) {
            rowStartIndex = rows.startSpan > 0 ? rowEndIndex - rows.startSpan : rowEndIndex - 1;
            rowStartIndex = qMax(0, rowStartIndex);
        }

        item.rowStart = rowStartIndex;
        if (rowStartIndex < 0)
            item.rowSpanHint = qMax(rows.startSpan, rows.endSpan);
        item.rowEnd = rowEndIndex;
    }

    // Pass 2: auto-place everything still unplaced, with a cursor that walks the
    // grid in the flow direction.
    //
    // The cursor must *wrap*: in row flow, once it passes the last column it
    // returns to the first column of the next row. Without the wrap an item with
    // an auto position always finds the next free cell to the right, so a grid
    // with two columns puts the third item in an implicit third column instead
    // of starting a second row - which is the difference between a two-column
    // grid and a single row that never ends.
    const bool columnFlow = containerStyle->gridAutoFlow.startsWith(QLatin1String("column"));

    // How far the cursor goes before it wraps. Row flow is bounded by the
    // template's column count, or by one column when there is no template, which
    // is what makes `display: grid` with no template a single-column grid.
    const int columnWrap = columnFlow ? 1 : qMax(1, explicitColumns);
    const int rowWrap = columnFlow ? qMax(1, explicitRows) : 1;

    {
        int cursorColumn = 0;
        int cursorRow = 0;

        const auto advance = [&] {
            if (columnFlow) {
                ++cursorRow;
                if (cursorRow >= rowWrap) {
                    cursorRow = 0;
                    ++cursorColumn;
                }
            } else {
                ++cursorColumn;
                if (cursorColumn >= columnWrap) {
                    cursorColumn = 0;
                    ++cursorRow;
                }
            }
        };

        for (GridItem &item : items) {
            const bool columnAuto = item.columnStart < 0;
            const bool rowAuto = item.rowStart < 0;
            if (!columnAuto && !rowAuto)
                continue;

            // The span comes from the hint when there is one: an item placed
            // only by `span 2` has no start or end line yet, so subtracting them
            // would give zero and the span would be lost.
            const int columnSpan = columnAuto
                ? std::max({1, item.columnSpanHint, item.columnEnd - item.columnStart})
                : 1;
            const int rowSpan = rowAuto
                ? std::max({1, item.rowSpanHint, item.rowEnd - item.rowStart})
                : 1;

            // An item with a definite column starts its search in that column,
            // which is what puts a sidebar item back in the sidebar rather than
            // wherever the cursor happens to be.
            if (!columnAuto)
                cursorColumn = item.columnStart;
            if (!rowAuto)
                cursorRow = item.rowStart;

            bool placed = false;
            // The bound is generous: a grid larger than this is pathological, and
            // running forever on a page that asks for one is worse than placing
            // the item anyway.
            for (int guard = 0; guard < 4096 && !placed; ++guard) {
                const int col = columnAuto ? cursorColumn : item.columnStart;
                const int row = rowAuto ? cursorRow : item.rowStart;

                bool collides = false;
                for (const GridItem &other : items) {
                    if (&other == &item || other.columnStart < 0 || other.rowStart < 0)
                        continue;
                    const bool overlapsColumns
                        = col < other.columnEnd && col + columnSpan > other.columnStart;
                    const bool overlapsRows
                        = row < other.rowEnd && row + rowSpan > other.rowStart;
                    if (overlapsColumns && overlapsRows) {
                        collides = true;
                        break;
                    }
                }

                if (!collides) {
                    if (columnAuto)
                        item.columnEnd = col + columnSpan;
                    if (rowAuto)
                        item.rowEnd = row + rowSpan;
                    item.columnStart = col;
                    item.rowStart = row;
                    placed = true;
                    break;
                }
                advance();
            }

            if (!placed) {
                // The search gave up. The item is placed at the cursor anyway so
                // that it is visible rather than silently dropped.
                item.columnStart = cursorColumn;
                item.rowStart = cursorRow;
                item.columnEnd = cursorColumn + columnSpan;
                item.rowEnd = cursorRow + rowSpan;
            }

            // The cursor steps past the item just placed, so the next one begins
            // where this one ended rather than on top of it.
            if (columnFlow)
                cursorRow = item.rowEnd;
            else
                cursorColumn = item.columnEnd;

            // A placement that lands exactly on the wrap boundary has to wrap
            // now, or the next item would start one column past the grid.
            if (columnFlow) {
                if (cursorRow >= rowWrap) {
                    cursorRow = 0;
                    ++cursorColumn;
                }
            } else if (cursorColumn >= columnWrap) {
                cursorColumn = 0;
                ++cursorRow;
            }
        }
    }

    // The grid grows to fit whatever the items needed, which is what makes
    // `grid-auto-rows` meaningful.
    for (const GridItem &item : items) {
        columnCount = qMax(columnCount, item.columnEnd);
        rowCount = qMax(rowCount, item.rowEnd);
    }
    if (columnCount <= 0)
        columnCount = 1;
    if (rowCount <= 0)
        rowCount = 1;

    // --------------------------------------------------- measure contributions
    //
    // Each item's intrinsic size feeds the tracks it spans. A spanning item
    // contributes its width and height to every track it crosses, which is an
    // approximation of the specification's distribution rule - it over-counts a
    // spanning item - but it keeps a multi-column card as wide as its content
    // instead of collapsing it.
    Context measuring = context;
    measuring.floats = nullptr;
    measuring.contentX = 0;

    for (GridItem &item : items) {
        measuring.availableWidth = qMax(0.0, context.availableWidth);

        // A specified width or height is what the item wants, and a shrink-to-fit
        // measures what it would take with the room available.
        const bool widthAuto = item.style->width.isAuto();
        const double paddingAndBorder = item.style->paddingLeft.resolve(0)
            + item.style->paddingRight.resolve(0) + item.style->borderLeftWidth
            + item.style->borderRightWidth;
        const double heightPaddingAndBorder = item.style->paddingTop.resolve(0)
            + item.style->paddingBottom.resolve(0) + item.style->borderTopWidth
            + item.style->borderBottomWidth;

        item.maxWidth = widthAuto ? shrinkToFitWidth(item.box, measuring)
                                  : resolveUsedWidth(item.box, context.availableWidth, nullptr)
                + paddingAndBorder;
        // The minimum is where a line could break if it had to: for text that is
        // the widest word, which the font metrics give.
        item.minWidth = widthAuto ? qMin(item.maxWidth, widestWordWidth(item.box, measuring))
                                  : item.maxWidth;

        // An item's height cannot be known without laying it out, so it is laid
        // out here, at the width this pass measured. The geometry is overwritten
        // by the placement pass below; what matters is the height it reports,
        // which is what sizes an `auto` row. Reading `box->height()` before this
        // ran was the bug that made every auto row zero-height, stacking the rows
        // on top of one another.
        Context probe = measuring;
        probe.contentX = 0;
        probe.contentY = 0;
        probe.availableWidth = qMax(0.0, item.maxWidth);
        probe.floats = nullptr;
        probe.depth = context.depth + 1;
        layoutBlock(item.box, probe, 0);

        const double specifiedHeight = resolveUsedHeight(item.box, measuring);
        item.maxHeight = specifiedHeight >= 0 ? specifiedHeight + heightPaddingAndBorder
                                              : item.box->height();
        item.minHeight = specifiedHeight >= 0 ? item.maxHeight : 0;
    }

    for (GridItem &item : items) {
        // CSS Grid §6.6: an item's *automatic* minimum is zero when it spans
        // more than one track in that axis. A track whose minimum the author
        // wrote out — `minmax(min-content, 1fr)` — keeps its floor regardless,
        // so only the `Auto` kind ignores a spanning item here.
        const bool singleColumn = item.columnEnd - item.columnStart == 1;
        const bool singleRow = item.rowEnd - item.rowStart == 1;

        for (int column = item.columnStart; column < item.columnEnd && column < columnCount;
             ++column) {
            while (columnTemplate.tracks.size() <= column) {
                css::GridTrack track;
                track.kind = css::GridTrack::Kind::Auto;
                columnTemplate.tracks.append(track);
            }
            css::GridTrack &track = columnTemplate.tracks[column];

            if (singleColumn || track.minKind != css::GridTrack::MinKind::Auto)
                track.contentMin = qMax(track.contentMin, item.minWidth);
            track.contentMax = qMax(track.contentMax, item.maxWidth);
        }
        for (int row = item.rowStart; row < item.rowEnd && row < rowCount; ++row) {
            while (rowTemplate.tracks.size() <= row) {
                css::GridTrack track;
                track.kind = css::GridTrack::Kind::Auto;
                rowTemplate.tracks.append(track);
            }
            css::GridTrack &track = rowTemplate.tracks[row];
            if (singleRow || track.minKind != css::GridTrack::MinKind::Auto)
                track.contentMin = qMax(track.contentMin, item.minHeight);
            track.contentMax = qMax(track.contentMax, item.maxHeight);
        }
    }

    // ------------------------------------------------------------- size tracks
    css::resolveTrackSizes(&columnTemplate.tracks, context.availableWidth, columnGap);

    // Rows are sized differently from columns, and the difference is not an
    // optimisation: a grid usually has no definite height, and then every row is
    // sized by its content and the container grows to hold them. Distributing an
    // invented "available height" across the rows instead - which is what
    // feeding `contentMax * rowCount` into the column algorithm does - shrinks
    // every row below its own content, so a two-row grid comes out with rows of
    // 14px holding 19px of text.
    //
    // A flexible row in an indefinite container is also content-sized, which is
    // what `1fr` means for a row in a grid that is only as tall as its content.
    if (context.availableHeight >= 0) {
        css::resolveTrackSizes(&rowTemplate.tracks, context.availableHeight, rowGap);
    } else {
        for (css::GridTrack &track : rowTemplate.tracks) {
            switch (track.kind) {
            case css::GridTrack::Kind::Fixed:
                track.size = qMax(0.0, track.value);
                break;
            case css::GridTrack::Kind::MinContent:
                track.size = track.minKind == css::GridTrack::MinKind::Fixed
                    ? qMax(0.0, track.floorPixels)
                    : qMax(0.0, track.contentMin);
                break;
            case css::GridTrack::Kind::FitContent:
                track.size = qMin(qMax(0.0, track.value), qMax(0.0, track.contentMax));
                break;
            case css::GridTrack::Kind::Auto:
            case css::GridTrack::Kind::MaxContent:
            case css::GridTrack::Kind::Fraction:
                // Content-sized: a row is as tall as the tallest item in it.
                track.size = qMax(0.0, track.contentMax);
                break;
            }
        }
    }

    // ------------------------------------------------------------- place items
    std::vector<double> columnOffsets(columnTemplate.tracks.size() + 1, 0);
    for (int i = 0; i < columnTemplate.tracks.size(); ++i)
        columnOffsets[i + 1] = columnOffsets[i] + columnTemplate.tracks.at(i).size + columnGap;

    std::vector<double> rowOffsets(rowTemplate.tracks.size() + 1, 0);
    for (int i = 0; i < rowTemplate.tracks.size(); ++i)
        rowOffsets[i + 1] = rowOffsets[i] + rowTemplate.tracks.at(i).size + rowGap;

    const double gridWidth = columnTemplate.tracks.isEmpty()
        ? 0
        : columnOffsets[columnTemplate.tracks.size()] - columnGap;
    const double gridHeight
        = rowTemplate.tracks.isEmpty() ? 0 : rowOffsets[rowTemplate.tracks.size()] - rowGap;

    // The whole track set is distributed when it is smaller than the container,
    // which is what `justify-content: center` means for a grid.
    double originX = context.contentX;
    const QString &justify = containerStyle->justifyContent;
    if (gridWidth < context.availableWidth) {
        const double free = context.availableWidth - gridWidth;
        if (justify == QLatin1String("center"))
            originX += free / 2;
        else if (justify == QLatin1String("flex-end") || justify == QLatin1String("end"))
            originX += free;
    }

    double originY = context.contentY;
    const QString &alignContent = containerStyle->alignContent;
    if (context.availableHeight >= 0 && gridHeight < context.availableHeight) {
        const double free = context.availableHeight - gridHeight;
        if (alignContent == QLatin1String("center"))
            originY += free / 2;
        else if (alignContent == QLatin1String("flex-end") || alignContent == QLatin1String("end"))
            originY += free;
    }

    const auto resolveAlign = [](const QString &value, const QString &fallback) {
        if (value.isEmpty() || value == QLatin1String("auto"))
            return fallback;
        return value;
    };

    double maxBottom = context.contentY;

    for (GridItem &item : items) {
        // The cell the item occupies, clamped to the tracks that exist.
        const int columnStart = qBound(0, item.columnStart, qMax(0, columnCount - 1));
        const int columnEnd = qBound(columnStart + 1, item.columnEnd, columnCount);
        const int rowStartIndex = qBound(0, item.rowStart, qMax(0, rowCount - 1));
        const int rowEndIndex = qBound(rowStartIndex + 1, item.rowEnd, rowCount);

        const double cellX = originX + columnOffsets[columnStart];
        const double cellY = originY + rowOffsets[rowStartIndex];
        const double cellWidth = columnOffsets[columnEnd] - columnOffsets[columnStart]
            - (columnEnd > columnStart ? columnGap : 0);
        const double cellHeight = rowOffsets[rowEndIndex] - rowOffsets[rowStartIndex]
            - (rowEndIndex > rowStartIndex ? rowGap : 0);

        // The item is laid out with its cell as the containing block, so a
        // percentage width resolves against the track rather than the grid.
        Context cell = context;
        cell.contentX = cellX;
        cell.contentY = cellY;
        cell.availableWidth = qMax(0.0, cellWidth);
        cell.availableHeight = qMax(0.0, cellHeight);
        cell.floats = nullptr;
        cell.depth = context.depth + 1;

        layoutBlock(item.box, cell, cellY);

        // Alignment inside the cell. `stretch` is the default and the reason a
        // grid item fills its track without any rule saying so.
        const QString justifySelf
            = resolveAlign(item.style->justifySelf, containerStyle->justifyItems);
        const QString alignSelfValue
            = resolveAlign(item.style->alignSelf, containerStyle->alignItems);

        double x = cellX;
        double y = cellY;
        double width = item.box->width();
        double height = item.box->height();

        const bool stretchWidth = justifySelf == QLatin1String("stretch")
            && item.style->width.isAuto();
        if (stretchWidth)
            width = cellWidth;
        else if (justifySelf == QLatin1String("center"))
            x += (cellWidth - width) / 2;
        else if (justifySelf == QLatin1String("end") || justifySelf == QLatin1String("flex-end"))
            x += cellWidth - width;

        const bool stretchHeight = alignSelfValue == QLatin1String("stretch")
            && item.style->height.isAuto() && context.availableHeight < 0;
        // A row sized to its content is exactly the item's height, so stretching
        // it vertically would only ever be a no-op. Stretching is therefore
        // limited to the width, which is the case that matters in practice.
        Q_UNUSED(stretchHeight);

        if (alignSelfValue == QLatin1String("center"))
            y += (cellHeight - height) / 2;
        else if (alignSelfValue == QLatin1String("end")
                 || alignSelfValue == QLatin1String("flex-end")) {
            y += cellHeight - height;
        }

        item.box->setPosition(x, y);
        item.box->setSize(width, height);
        maxBottom = qMax(maxBottom, y + height);
    }

    return maxBottom - context.contentY;
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

    // ------------------------------------------------- formatting context
    //
    // Floats belong to a block formatting context and escape their parent unless
    // the parent establishes one of its own. A block establishes a new one when
    // it is a flex container, or when it is scrollable - `overflow` other than
    // `visible` - and that is what makes `overflow: hidden` the classic way to
    // stop a container collapsing around its floated children.
    //
    // A box that does not establish one shares its parent's, which is why a
    // float in one paragraph still pushes the next paragraph's text aside.
    FloatContext ownFloats;
    const bool establishesFlow = style->isFlexContainer() || style->isGridContainer()
        || box->type() == Box::Type::InlineBlock
        || (style->overflow != QLatin1String("visible") && box->type() == Box::Type::Block);

    Context inner;
    inner.depth = context.depth + 1;
    inner.contentX = contentX;
    inner.contentY = contentY;
    inner.availableWidth = contentWidth;
    inner.floats = establishesFlow ? &ownFloats : context.floats;
    inner.flowX = establishesFlow ? contentX : context.flowX;
    inner.flowWidth = establishesFlow ? contentWidth : context.flowWidth;

    // The root has no enclosing context, so it is one: without this a float at
    // the top of a document would have nothing to be registered in and every
    // float on the page would be inert.
    if (!inner.floats) {
        inner.floats = &ownFloats;
        inner.flowX = contentX;
        inner.flowWidth = contentWidth;
    }

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
        // An out-of-flow child is laid out by the positioned pass and adds no
        // height here, so a container whose only block-level child is out of
        // flow falls through to the inline path and sizes from its text.
        bool hasBlockChild = false;
        for (const auto &child : box->children()) {
            if (child->isBlockLevel() && !child->isOutOfFlow())
                hasBlockChild = true;
        }

        // A flex or grid container lays its children out along its own rules
        // instead of stacking them, so it takes neither the block nor the inline
        // path.
        if (style->isFlexContainer())
            contentHeight = layoutFlexChildren(box, inner);
        else if (style->isGridContainer())
            contentHeight = layoutGridChildren(box, inner);
        else if (hasBlockChild)
            contentHeight = layoutBlockChildren(box, inner, &trailingMargin);
        else
            contentHeight = layoutInlineRun(box, inner);
    }

    // A formatting context contains its floats: the height of the box grows to
    // reach the lowest float it holds, so `overflow: hidden` around floated
    // children makes the parent tall enough for them. This is the mechanism that
    // makes the classic clearfix unnecessary.
    if (establishesFlow && !ownFloats.bands.empty())
        contentHeight = qMax(contentHeight, ownFloats.bottom() - contentY);

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

    // A relative offset is applied once the box has its final geometry, and the
    // value returned below is computed from the un-displaced position, so the
    // flow around the box is untouched. That is what separates relative from
    // absolute positioning: the space it would have used stays reserved.
    applyRelativeOffset(box);

    // The bottom margin collapses with the parent's, so it is not part of the
    // height returned to the caller; the caller adds the collapsed amount.
    return borderTopY + borderBoxHeight + trailingMargin;
}

// --------------------------------------------------------------- positioning

bool LayoutEngine::relativeOffsetFor(const Box *box, double containingBlockWidth,
                                     double containingBlockHeight, double *dx, double *dy) const
{
    const css::ComputedStyle *style = box->style();
    if (!style || style->position != QLatin1String("relative"))
        return false;

    // Only the properties the author actually wrote displace the box, and each
    // axis is decided on its own: `left` wins over `right`, `top` over `bottom`,
    // which is the CSS 2.2 rule for over-constrained offsets.
    const bool hasLeft = !style->left.isAuto();
    const bool hasRight = !style->right.isAuto();
    const bool hasTop = !style->top.isAuto();
    const bool hasBottom = !style->bottom.isAuto();

    if (!hasLeft && !hasRight && !hasTop && !hasBottom)
        return false;

    double localX = 0;
    if (hasLeft)
        localX = style->left.resolve(containingBlockWidth);
    else if (hasRight)
        localX = -style->right.resolve(containingBlockWidth);

    double localY = 0;
    if (hasTop)
        localY = style->top.resolve(containingBlockHeight);
    else if (hasBottom)
        localY = -style->bottom.resolve(containingBlockHeight);

    *dx = localX;
    *dy = localY;
    return true;
}

void LayoutEngine::applyRelativeOffset(Box *box)
{
    // A percentage offset on a relatively positioned box resolves against the
    // containing block's width for both axes, which is the CSS 2.2 rule that
    // surprises people: `top: 50%` of a 800px-wide block is 400px, not half its
    // height.
    Box *containing = box->containingBlock();
    const double width = containing ? containing->contentBox().width() : m_viewportWidth;
    const double height = containing ? containing->contentBox().height() : m_viewportHeight;

    double dx = 0;
    double dy = 0;
    if (!relativeOffsetFor(box, width, height, &dx, &dy))
        return;

    // The displacement carries the subtree, and it happens after the box's own
    // height was reported to its parent, so a relative offset never disturbs the
    // flow around it. That is the whole difference between relative and
    // absolute positioning.
    box->translate(dx, dy);
}

void LayoutEngine::layoutAbsolutelyPositioned(Box *box, int depth)
{
    const css::ComputedStyle *style = box->style();
    if (!style)
        return;

    // The containing block is the padding box of the nearest positioned
    // ancestor, or the viewport when there is none. For a `fixed` box the
    // viewport always applies, because fixed positioning is relative to the
    // viewport by definition.
    Box *ancestor = style->position == QLatin1String("fixed") ? nullptr : box->positionedAncestor();

    // The area the box is placed inside. For an ancestor this is its padding
    // box, which is what `top: 0` is measured from.
    // A fixed box is always placed against the viewport, and so is an absolute
    // box with no positioned ancestor. A percentage width or height then
    // resolves against the viewport, which is what makes
    // `position: fixed; width: 100%; height: 100%` fill the screen.
    const QRectF containingRect = ancestor
        ? ancestor->paddingBox()
        : QRectF(0.0, 0.0, m_viewportWidth,
                 m_viewportHeight > 0 ? m_viewportHeight : m_viewportWidth);

    // The offsets, resolved against the containing block's size.
    const bool hasLeft = !style->left.isAuto();
    const bool hasRight = !style->right.isAuto();
    const bool hasTop = !style->top.isAuto();
    const bool hasBottom = !style->bottom.isAuto();

    const Edges padding = paddingOf(box, containingRect.width());
    const Edges border = borderOf(box);
    const double paddingBorderX = padding.left + padding.right + border.left + border.right;
    const double paddingBorderY = padding.top + padding.bottom + border.top + border.bottom;

    /// The height the box will be given, or -1 when it sizes from its content.
    /// It is settled before the layout because a percentage height has to be
    /// resolved against the containing block, and a box pinned by both `top` and
    /// `bottom` takes the distance between them.
    double claimedHeight = -1;

    // ------------------------------------------------------------- width
    //
    // A box pinned on both sides takes the distance between them, which is how
    // `left: 0; right: 0` makes a full-width overlay. Otherwise it shrinks to
    // fit its content, which is the other half of what absolute positioning is
    // used for.
    bool widthAuto = style->width.isAuto();
    double contentWidth = 0;
    bool widthFromOffsets = false;

    if (!widthAuto) {
        contentWidth = style->width.resolve(containingRect.width());
        if (style->boxSizing == QLatin1String("border-box"))
            contentWidth -= paddingBorderX;

        if (!style->height.isAuto() && style->height.isPercentage() && claimedHeight < 0)
            claimedHeight = style->height.resolve(containingRect.height());
    } else if (hasLeft && hasRight) {
        const double leftOffset = style->left.resolve(containingRect.width());
        const double rightOffset = style->right.resolve(containingRect.width());
        contentWidth = containingRect.width() - leftOffset - rightOffset - paddingBorderX;
        widthFromOffsets = true;
    } else {
        // Shrink to fit: the widest line the content would produce, clamped to
        // the containing block. An absolutely positioned box never fills its
        // container the way a block-level one would.
        const double available = qMax(0.0, containingRect.width() - paddingBorderX);
        Context measuring;
        measuring.depth = 0;
        measuring.contentX = 0;
        measuring.contentY = 0;
        measuring.availableWidth = available;
        measuring.availableHeight = -1;
        contentWidth = shrinkToFitWidth(box, measuring);
        widthAuto = true;
    }

    contentWidth = qMax(0.0, contentWidth);

    if (!style->minWidth.isAuto())
        contentWidth = qMax(contentWidth, style->minWidth.resolve(containingRect.width())
                                             - (style->boxSizing == QLatin1String("border-box")
                                                    ? paddingBorderX
                                                    : 0.0));
    if (!style->maxWidth.isAuto())
        contentWidth = qMin(contentWidth, style->maxWidth.resolve(containingRect.width())
                                             - (style->boxSizing == QLatin1String("border-box")
                                                    ? paddingBorderX
                                                    : 0.0));

    // ------------------------------------------------------------- layout
    //
    // A box pinned by both `top` and `bottom` takes the distance between them,
    // which is how a panel is stretched to a height it does not state.
    if (hasTop && hasBottom && style->height.isAuto()) {
        const double topOffset = style->top.resolve(containingRect.height());
        const double bottomOffset = style->bottom.resolve(containingRect.height());
        claimedHeight = qMax(0.0, containingRect.height() - topOffset - bottomOffset
                                      - paddingBorderY);
    }

    Context abs;
    abs.depth = depth;
    abs.contentX = 0;
    abs.contentY = 0;
    abs.availableWidth = contentWidth;
    abs.availableHeight = claimedHeight >= 0 ? claimedHeight : -1;

    // The style is overridden for the duration so that layoutBlock uses the
    // width this pass resolved rather than re-deriving it, exactly as the flex
    // item path does.
    const css::ComputedStyle *original = box->style();
    css::ComputedStyle sized = *original;
    sized.boxSizing = QStringLiteral("content-box");
    sized.width = css::LengthOrAuto::pixels(contentWidth);
    sized.marginLeft = css::LengthOrAuto::pixels(0);
    sized.marginRight = css::LengthOrAuto::pixels(0);
    if (claimedHeight >= 0)
        sized.height = css::LengthOrAuto::pixels(claimedHeight);

    // The copy is kept in the arena because the boxes built during this layout -
    // line boxes in particular - hold the style pointer they were made with.
    box->setStyle(arenaStyle(sized));

    layoutBlock(box, abs, 0);

    box->setStyle(original);

    const double borderBoxWidth = box->borderBox().width();
    const double borderBoxHeight = box->borderBox().height();

    // ----------------------------------------------------------- position
    //
    // Horizontal first: `left` wins when both are given, which is the CSS 2.2
    // rule for an over-constrained box. When neither is given the box stays at
    // its static position, which is where normal flow would have put it; the
    // static position is approximated by the containing block's content edge,
    // since the box never took part in flow.
    double x = containingRect.x();
    if (hasLeft)
        x = containingRect.x() + style->left.resolve(containingRect.width());
    else if (hasRight)
        x = containingRect.right() - style->right.resolve(containingRect.width())
            - borderBoxWidth;

    double y = containingRect.y();
    if (hasTop)
        y = containingRect.y() + style->top.resolve(containingRect.height());
    else if (hasBottom)
        y = containingRect.bottom() - style->bottom.resolve(containingRect.height())
            - borderBoxHeight;

    Q_UNUSED(widthAuto);
    Q_UNUSED(widthFromOffsets);

    placeBoxAt(box, x, y);
}

void LayoutEngine::layoutAbsoluteDescendants(Box *box, int depth)
{
    // Depth first, so a box is placed before the boxes it contains. An absolute
    // descendant of an absolute box is positioned against that box, which is why
    // this cannot be a single flat pass over the tree: the ancestor has to have
    // its final geometry before the descendant can be placed.
    for (const auto &childPtr : box->children()) {
        Box *child = childPtr.get();
        if (!child)
            continue;

        const css::ComputedStyle *style = child->style();
        if (!style || !style->generatesBox())
            continue;

        if (child->isOutOfFlow()) {
            // The box is placed first, because a descendant of its own is
            // positioned against it and needs its final geometry. Then its own
            // absolute descendants are placed against it.
            layoutAbsolutelyPositioned(child, depth + 1);
            layoutAbsoluteDescendants(child, depth + 1);
        } else {
            layoutAbsoluteDescendants(child, depth + 1);
        }
    }
}

const css::ComputedStyle *LayoutEngine::arenaStyle(const css::ComputedStyle &style)
{
    m_styleArena.push_back(style);
    return &m_styleArena.back();
}

LayoutResult LayoutEngine::layout(Box *root)
{
    m_result = LayoutResult();

    // The overrides the previous pass handed out belong to the box tree that
    // pass produced, which is being replaced.
    m_styleArena.clear();

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

    // Absolutely positioned boxes are placed after normal flow, because their
    // containing block has to have its final size first: `bottom: 0` needs the
    // height, and the height depends on the content that flowed.
    layoutAbsoluteDescendants(root);

    // The document is as tall as the deepest content, which is not always the
    // root box itself: a child can overflow its parent when the parent has a
    // specified height, and a scrolling page must still reach that content.
    double contentBottom = root->y() + root->height();
    std::function<void(Box *)> widenForContent = [&](Box *box) {
        contentBottom = qMax(contentBottom, box->borderBox().bottom());
        for (const auto &child : box->children())
            widenForContent(child.get());
    };
    widenForContent(root);

    m_result.documentWidth = qMax(m_viewportWidth, root->x() + root->width());
    m_result.documentHeight = qMax(contentBottom, m_viewportHeight);

    return m_result;
}

} // namespace oqb::renderer
