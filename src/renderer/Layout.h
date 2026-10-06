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
/// inline line breaking, margin collapsing, flexbox, relative and absolute
/// positioning, replaced elements and box-sizing. Features outside that set -
/// floats, grid, table layout - have no effect at all, and rendering.md lists
/// them so a reader knows what to expect rather than assuming they work.
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

    /// Lays out a flex container's children along its main and cross axes and
    /// returns the content height. Flex layout is not block layout with a
    /// different gap: the items' sizes feed back into each other through the
    /// free space, so the whole row (or column) is measured before anything is
    /// placed. See architecture/rendering.md for the algorithm.
    double layoutFlexChildren(Box *box, const Context &context);

    /// One flex item, as the sizing and placing passes see it.
    struct FlexItem
    {
        Box *box = nullptr;
        /// The style the item is laid out with, which is its own.
        const css::ComputedStyle *style = nullptr;

        /// The item's outer size on the main axis, before free space is
        /// distributed. This is its flex basis resolved to a number, or its
        /// content size when the basis is auto.
        double baseSize = 0;
        /// The size after grow and shrink are applied. This is what the item is
        /// given on the main axis.
        double mainSize = 0;
        /// The size on the cross axis, either the specified size or, after the
        /// second pass, the item's own content size.
        double crossSize = 0;

        /// Outer margins on the main axis. Auto margins absorb free space, which
        /// is how a flex item is pushed to one end.
        double marginMainBefore = 0;
        double marginMainAfter = 0;
        /// Auto margins on the cross axis, which centre the item.
        double marginCrossBefore = 0;
        double marginCrossAfter = 0;

        /// Border and padding on the main axis, so the content size is what the
        /// item is actually laid out at.
        double mainPaddingBorder = 0;
        double crossPaddingBorder = 0;

        /// The flex factors, copied off the style so the passes do not consult
        /// the cascade repeatedly.
        double grow = 0;
        double shrink = 1;
        /// The resolved align-self, which falls back to the container's
        /// align-items when the item's own value is auto.
        QString alignSelf;
        /// The item's order property, which decides the sequence the items are
        /// placed in.
        int order = 0;
        /// The item's position on the main axis, filled in by the placing pass.
        double mainPosition = 0;
        /// The item's position on the cross axis.
        double crossPosition = 0;
        /// The item's resulting outer cross size, including its margins.
        double outerCrossSize = 0;
    };

    /// Measures and lays out one flex item at a given main-axis size.
    /// `definiteCross` is the cross size to lay the item out at, or a negative
    /// number when the item sizes itself.
    void layoutFlexItem(FlexItem &item, const Context &context, double mainSize,
                        double definiteCross, double mainOrigin, double crossOrigin,
                        bool rowDirection);

    /// The resolved value of align-self for an item: its own, or the
    /// container's align-items when the item says auto.
    static QString resolveAlignSelf(const FlexItem &item, const css::ComputedStyle *container);

    /// Reserves a style that overrides part of a box's own, for the duration of
    /// the layout.
    ///
    /// Flex items and absolutely positioned boxes are laid out at a size this
    /// engine resolved rather than at the one their own `width` asks for, so they
    /// are given a copy of their style with that size substituted. The copy has
    /// to outlive the pass: layout stores style pointers in the boxes it builds,
    /// including on line boxes, and those are still read when the page is
    /// painted. The arena gives every copy a stable address for as long as the
    /// box tree built by this layout lives.
    const css::ComputedStyle *arenaStyle(const css::ComputedStyle &style);

    /// Every style this layout pass created. Deque rather than vector, because
    /// the addresses are handed out and must survive later insertions.
    std::deque<css::ComputedStyle> m_styleArena;

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

    /// Applies a relatively positioned box's offsets. Relative positioning is
    /// the one kind that does not remove a box from flow: the space it would
    /// have occupied is still reserved, and only the box's own geometry moves.
    /// Applying it at the end of the box's own layout is therefore correct, and
    /// the ancestor's cursor is unaffected because the move happens after the
    /// height has been reported.
    void applyRelativeOffset(Box *box);

    /// The distance a relatively positioned box is displaced by, resolved.
    /// Returns false when the box is not relatively positioned, or when it has
    /// no offset at all, which is the common case and worth skipping.
    bool relativeOffsetFor(const Box *box, double containingBlockWidth,
                           double containingBlockHeight, double *dx, double *dy) const;

    /// Lays out every absolutely positioned descendant of `box` that has not yet
    /// been placed. Absolute boxes are laid out after normal flow, because their
    /// containing block is only known once its own height and position are, and
    /// because `bottom: 0` needs the container's final height.
    void layoutAbsoluteDescendants(Box *box, int depth = 0);
    /// Lays out one absolutely positioned box against its containing block.
    /// `depth` is threaded through from the enclosing pass so that the recursion
    /// guard still applies: resetting it would let a deeply nested or cyclic
    /// arrangement of positioned boxes exhaust the stack.
    void layoutAbsolutelyPositioned(Box *box, int depth);

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
