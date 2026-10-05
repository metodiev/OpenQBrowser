#pragma once

#include <QFont>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <vector>

#include "renderer/BoxTree.h"

namespace oqb::renderer {

/// The outcome of laying out a document.
struct LayoutResult
{
    /// Total height of the laid-out content: how far the page scrolls.
    double documentHeight = 0;
    /// Width the layout was performed at.
    double documentWidth = 0;
    /// Every line box produced, in document order. Hit testing and the
    /// headless text dump both walk this list.
    std::vector<Box *> lineBoxes;
    /// Features the engine could not honour, surfaced by the inspector.
    QStringList warnings;

    /// Finds the deepest box whose border box contains `point`, or nullptr.
    Box *hitTest(double x, double y) const;
};

/// Lays out a box tree in the CSS 2.2 sense.
///
/// Every box's geometry is stored in absolute document coordinates, which keeps
/// the engine free of coordinate translations and lets the painter draw a box
/// without walking its ancestors. The implementation covers block stacking,
/// inline line breaking, margin collapsing, floats, table display types and
/// replaced elements; features outside that set are recorded as warnings so the
/// inspector can report them instead of silently mis-rendering.
class LayoutEngine
{
public:
    /// The greatest element nesting layout will descend through. A page cannot
    /// nest deeper than this before layout stops honouring it, which is the
    /// honest alternative to running out of stack: browsers impose a limit for
    /// the same reason, and markup generated rather than written can nest
    /// thousands of levels deep.
    static constexpr int kMaxLayoutDepth = 500;

    LayoutEngine() = default;

    void setViewport(double width, double height);

    /// Lays out `root` at the viewport size and returns the document metrics.
    LayoutResult layout(Box *root);

    /// The font a style resolves to. The painter needs the same answer, so the
    /// resolution lives here rather than in either consumer.
    static QFont fontFor(const css::ComputedStyle *style);

    double viewportWidth() const { return m_viewportWidth; }
    double viewportHeight() const { return m_viewportHeight; }

private:
    /// Where a box is being laid out and how much room it has.
    struct Context
    {
        /// How many ancestors have been laid out already.
        int depth = 0;

        /// Absolute x of the containing block's content edge.
        double contentX = 0;
        /// Absolute y of the containing block's content edge.
        double contentY = 0;
        /// Width available for the content.
        double availableWidth = 0;
        /// Height of the containing block, or -1 when it depends on content.
        double availableHeight = -1;
    };

    /// Lays out one block-level box at an absolute position and returns the
    /// distance from its top margin edge to the next box's top margin edge.
    double layoutBlock(Box *box, const Context &context, double borderTopY);

    /// Lays out a block container's children sequentially. Returns the total
    /// content height, with the trailing margin reported through `lastMargin`.
    double layoutBlockChildren(Box *box, const Context &context, double *lastMargin);

    /// The resolved bottom margin a box contributes to the gap before the next
    /// sibling. layoutBlock() folds it into its return value; the caller needs
    /// it separately to collapse it with the next sibling's top margin.
    double trailingMarginOf(const Box *box, const Context &context) const;

    /// Builds line boxes for inline content. Returns the content height, and
    /// reports through `widestLine` the widest line actually produced, which is
    /// what an auto-width inline-block uses to shrink to fit its content.
    double layoutInlineRun(Box *container, const Context &context, double *widestLine = nullptr);

    /// The shrink-to-fit width of a box's inline content: the widest line it
    /// would produce without wrapping, clamped to the available width.
    double shrinkToFitWidth(Box *box, const Context &context);

    /// Moves a box so its border box starts at (x, y), carrying its subtree.
    static void placeBoxAt(Box *box, double x, double y);

    /// Lays out a replaced box such as <img> from its intrinsic size.
    void layoutReplaced(Box *box, const Context &context);

    double resolveUsedWidth(const Box *box, double containingBlockWidth, bool *isAuto) const;
    double resolveUsedHeight(const Box *box, const Context &context) const;

    /// Border and padding in pixels, resolved against the containing block.
    struct Edges
    {
        double top = 0;
        double right = 0;
        double bottom = 0;
        double left = 0;
    };
    Edges paddingOf(const Box *box, double containingBlockWidth) const;
    Edges borderOf(const Box *box) const;

    double m_viewportWidth = 1024;
    double m_viewportHeight = 768;
    LayoutResult m_result;
};

} // namespace oqb::renderer
