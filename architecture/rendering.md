# Rendering

`src/renderer/` turns a styled `dom::Document` into pixels. It has three stages,
each in its own pair of files:

| File | Contents |
| --- | --- |
| `src/renderer/BoxTree.h`, `BoxTree.cpp` | `renderer::Box` and `renderer::BoxTreeBuilder`. |
| `src/renderer/Layout.h`, `Layout.cpp` | `LayoutEngine`, `LayoutResult`, `LayoutEngine::Context`. |
| `src/renderer/Painter.h`, `Painter.cpp` | `Painter`, `Painter::Options`. |

`Page::buildLayout()` drives them in order: `StyleEngine::computeStyles()` →
`BoxTreeBuilder::setStyleEngine()` + `build()` → `LayoutEngine::layout()`. The
same three calls appear in `tests/unit/tst_layout.cpp` and
`tests/integration/tst_pipeline.cpp`, which is what makes a headless layout
identical to a windowed one.

## The box tree

A `Box` is created by `BoxTreeBuilder::buildForNode()` for every element that
generates one, and is one of these types:

| Type | Produced for |
| --- | --- |
| `Block` | `display: block`, `list-item`, `table*`, `flex` — anything `ComputedStyle::isBlockLevel()` accepts. |
| `Inline` | `display: inline`. |
| `InlineBlock` | `display: inline-block`. |
| `Text` | A text node; its content is the parent element's style. |
| `Replaced` | `img`, `image`, `video`, `audio`, `canvas`, `iframe`, `embed`, `object`, `input`, `select`, `textarea`, `svg`, `math`. |
| `Line` | Created by inline layout to hold the fragments of one line. |
| `Anonymous` | Inserted so a block container never mixes block and inline children (below). |
| `Bullet` | The marker of a list item, created in `LayoutEngine::layoutBlock()`. |

Each box keeps a raw `dom::Node *` (`node()`), a raw `const css::ComputedStyle *`
(`style()`) and a pointer to its parent. Children are owned through
`std::vector<std::unique_ptr<Box>>`; `appendChild()`, `detachChild()` and
`clearChildren()` are the mutators. Comments and doctypes generate no boxes, and
`isNonRenderedElement()` drops `head`, `title`, `meta`, `link`, `style`, `script`,
`base`, `template`, `noscript`, `param`, `source`, `track` and `datalist`
regardless of their `display`.

The style pointer is a raw pointer into `StyleEngine`'s map, so the engine must
outlive the tree; `BoxTreeBuilder::setStyleEngine()` records it in a file-local
`g_styleEngine`, and the comment on that variable states the lifetime rule.
Geometry accessors are `borderBox()`, `contentBox()`, `paddingBox()` and
`marginBox()`, plus `setPosition()`, `setSize()` and `translate()`, which moves a
box and its whole subtree.

### Anonymous boxes, and why they are needed

CSS 2.2 §9.2.1.1 requires that a block container either holds only block-level
children or establishes an inline formatting context for its inline children —
never both at once. `BoxTreeBuilder::buildForNode()` enforces that while walking
the children: consecutive inline children accumulate in a local `inlineRun` list,
which is flushed into an `Anonymous` box whenever a block-level sibling appears
or the children end. The wrapper is created only when the container is itself a
`Block` or an `InlineBlock`, because those establish their own formatting context
(this is why `PageView`'s generated pages and an inline-block badge both work).
A plain `Inline` box keeps its children as they are, so text can flow through a
`<span>` into the enclosing line.

The effect is visible in the geometry dump from `--dump-boxes` or
`--dump-layout`: `<p>text</p>` produces `block p` → `anonymous` → `line` → `text`.
`tests/unit/tst_layout.cpp::createsAnonymousBlocks()` asserts the wrapper exists.

Whitespace-only text nodes between block siblings are dropped before this
(`isIgnorableWhitespace()`), otherwise a paragraph of indentation in the source
would produce anonymous blocks with no content.

Text is transformed at this point too: `transformText()` collapses whitespace with
`QString::simplified()` unless `white-space` is `pre` or `pre-wrap`, and applies
`text-transform: uppercase/lowercase/capitalize`.

## Layout

`LayoutEngine::layout(root)` returns a `LayoutResult` with `documentWidth`,
`documentHeight`, `lineBoxes` (every line in document order, used by hit testing
and the text dump) and `warnings`.

### Absolute coordinates

**Every box is stored with absolute document coordinates.** The private `Context`
carries the containing block's content origin (`contentX`, `contentY`),
`availableWidth`, and `availableHeight` (`-1` when it depends on content). Layout
functions receive it and compute a child's absolute position directly, so
`placeBoxAt(box, x, y)` can move a finished subtree, and the painter never walks
ancestors to find out where to draw. The trade-off is that percentage resolution
always needs the containing block's width passed down.

### Block formatting and stacking

`layoutBlock(box, context, borderTopY)` is the core:

1. Padding and border come from `paddingOf()` and `borderOf()`; an anonymous box
   uses empty edges because the decoration belongs to its parent.
2. Width: `resolveUsedWidth()` returns `availableWidth` for `auto`, otherwise the
   resolved length clamped by `min-width`/`max-width`. Percentages resolve against
   the containing block's content width — as do percentage padding and margins,
   including the vertical ones; only a specified `height` percentage uses
   `availableHeight`, which is -1 (resolving to 0) until a parent has one.
3. Horizontal margins: a fixed margin is resolved inline; `auto` margins absorb
   the leftover space — both auto split it evenly (the `margin: 0 auto` centring
   idiom), one auto takes all of it, no auto leaves the space unused on the right.
   `tests/unit/tst_layout.cpp::centresWithAutoMargins()` asserts the arithmetic.
4. Children: if any child is block-level, `layoutBlockChildren()` stacks them;
   otherwise `layoutInlineRun()` builds line boxes. The two are exclusive because
   the box tree already separated the runs.
5. Height: content height, unless a specified height exists, in which case it
   wins (CSS 2.2 §10.6.3).
6. Geometry is written to the box, and the return value is the y past the box's
   bottom margin — which is what lets the caller collapse the next sibling's top
   margin against it.

`layoutBlock()` also creates the list marker: for a `list-item` block whose
`list-style-position` is `outside`, it registers a per-parent counter in a
`thread_local QHash` and appends a `Bullet` box carrying the marker text from
`markerFor()` (`•`, `◦`, `▪`, `1.`, `a.`, `A.`, or lower/upper Roman numerals up
to 3999).

### Margin collapsing between siblings

`layoutBlockChildren()` uses one variable, `collapsedMargin`:

```
y = contentY; collapsedMargin = 0
for each block child:
    gap = max(collapsedMargin, child.marginTop)      // the larger margin wins
    y += gap
    y = layoutBlock(child, ...) - trailingMarginOf(child)   // child's own bottom margin removed
    collapsedMargin = child.marginBottom
```

The gap between two boxes is therefore the larger of the two adjacent margins, not
their sum, as CSS 2.2 §8.3.1 requires. The last child's bottom margin is reported
through `lastMargin` rather than applied, so it can collapse with the parent's.
`tests/unit/tst_layout.cpp::collapsesAdjacentMargins()` asserts both the equal and
the unequal cases.

Only sibling collapsing is implemented. Parent/first-child and empty-block
collapsing, and the "collapsing through" rules for boxes with zero height, are not:
the parent's top edge stays where the first child's collapsed top margin puts it,
and the root's own top margin is applied by `layout()`.

### Inline layout and line breaking

`layoutInlineRun(container, context, widestLine)` has three phases.

**Measure.** A recursive `collect()` flattens the inline subtree into `Fragment`s:
the text, the `ComputedStyle*`, a `spaceBefore` flag (the spaces *are* the break
opportunities, not content), a `breakBefore` flag for `<br>`, `width`, `height`,
`baseline` and `lineHeight`. Text is split on spaces into words and measured with
`QFontMetricsF`. Replaced boxes are laid out immediately, because their size is
needed for the line. An `inline-block` is measured with `shrinkToFitWidth()` when
its width is auto and laid out as a block; it becomes one atomic placeholder.

**Detach and release.** The measured inline children are removed from the
container, except replaced boxes and inline-blocks, which are kept alive because
they own real content and subtrees. The rest are destroyed; leaving them would let
the painter draw unpositioned text at the container origin.

**Build lines.** A fragment that would exceed `availableWidth` starts a new line,
unless it is the first on the line — a long word overflows rather than
disappearing, which `tests/unit/tst_layout.cpp::wrapsLongTextIntoLines()` asserts.
A `Line` box is created per line, `lineAscent`/`lineHeight` are the maxima over the
line's fragments, and each text fragment becomes a `Text` box positioned so its
ascent lands on the line's baseline (`y + max(0, lineAscent - fragment.baseline)`),
which keeps mixed font sizes sitting on one line. A line with no content still
advances by its height, so an empty paragraph or a `<br>` takes space. Atomic boxes
are re-attached at the line's current x: a replaced box is centred vertically, an
inline-block sits with its own baseline on the line's. `widestLine` reports the
width the content would have taken without wrapping, which is what an auto-width
inline-block shrinks to fit.

### Shrink-to-fit

`shrinkToFitWidth(box, context)` measures the deepest text width in the subtree
using the font metrics, adds the inline padding/border/margin indents seen along
the way, adds the box's own left/right padding and border, and clamps the result
to `context.availableWidth`. It is used for auto-width inline-blocks and for
floats that have no width of their own. After the clamp, the box is laid out as a
block at that width and becomes an atomic inline.

### Replaced elements

`layoutReplaced(box, context)` implements CSS 2.2 §10.3.2: with both dimensions
auto the intrinsic size is used, with one auto the intrinsic ratio decides it, and
with both specified both are respected. Intrinsic sizes come from
`Box::intrinsicSize()`, which `Page::handleSubresource()` fills in from a decoded
image. Without one, the defaults are `input` 173×21, `textarea` 173×64,
`iframe`/`canvas`/`video` 300×150, and 0×0 otherwise — so a missing image is
invisible rather than taking space.

### Nesting limits

Two depth caps protect the engine from markup that is generated rather than
written:

* `html::ParseOptions::maxDepth` (400) truncates tree construction: a start tag
  past the limit is dropped with the warning `"Maximum element depth reached at
  <x>"`, so the DOM cannot be deeper than that.
* `LayoutEngine::kMaxLayoutDepth` (500, in `Layout.h`) stops `layoutBlock()`: the
  box gets its position and a zero content height, and `LayoutResult::warnings`
  records the nesting limit once. The document still lays out; the deep content is
  clipped rather than crashing the recursion.

`tests/integration/tst_redirect.cpp::survivesDeeplyNestedMarkup()` exercises
nesting 10, 50, 120 and 200 blocks deep through the whole parse → style → box →
layout chain.

### Table display types

The display types `table`, `table-row`, `table-cell`, `table-row-group`,
`table-header-group`, `table-footer-group`, `table-caption`, `table-column` and
`table-column-group` are parsed, stored in `ComputedStyle::display`, and mapped by
`isBlockLevel()` so they generate boxes. **There is no table layout algorithm.**
Rows and cells lay out as ordinary stacked blocks at full width. `border-collapse`
is stored but never read by `src/renderer/`; `border-spacing` appears only in the
user agent stylesheet text and is not parsed into `ComputedStyle` at all. A simple
`<table>` with plain text cells still renders; column widths, alignment and
collapsed borders do not.

### What layout leaves alone

`LayoutResult::warnings` carries the one diagnostic the engine produces: the
nesting-limit warning described above, recorded once per layout. Everything else
in this section is simply absent — these properties are stored in `ComputedStyle`
and never read by `src/renderer/`. Positioning, flexbox and `box-sizing` are
implemented and documented below, so they are not listed here.

| Property | What you observe |
| --- | --- |
| `float`, `clear` | **Implemented.** A float shrinks to fit, is pushed aside by the floats before it, and shortens the lines that run beside it. `clear` moves a box below the floats it names. See the Floats section below. |
| `vertical-align` | Lines are always baseline-aligned. |
| `text-align` | A line always starts at the containing block's left content edge. |
| `overflow` | Nothing clips; no scroll container is established inside a page. |
| `display: grid` | Recognised but laid out as ordinary blocks. |
| `display: table*` | Rows and cells stack as ordinary blocks at full width. |

A reader should expect these features to have **no layout effect** rather than to
produce a warning of their own.

## Flexbox

`display: flex` is laid out by `LayoutEngine::layoutFlexChildren()`, reached from
the same dispatch in `layoutBlock()` that chooses between block and inline
layout. It follows the specification's structure rather than being a pile of
special cases, because the parts of flexbox that look like details — growing,
shrinking, wrapping, alignment — are all consequences of two numbers: each
item's **base size** and the **free space** left in its line.

### The passes

1. **Collect.** The container's children become items, in document order.
   Absolutely positioned children are skipped, since they are out of flow. The
   items are then stably sorted by `order`, which is what `order: -1` uses to pull
   an item to the front without moving it in the DOM.

2. **Base size.** Each item's base size is its `flex-basis`, or its own
   `width`/`height` when the basis is auto, or finally its measured content size.
   A definite basis wins over the item's width, which is what makes
   `flex: 0 0 150px` give equal columns regardless of content. Sizing from
   content is the expensive case: the item has to be laid out once to find out
   how big it wants to be, and that measurement is thrown away.

3. **Lines.** With `flex-wrap`, items go into lines that fit the container's main
   size. An item that alone exceeds the container still starts a line, which is
   what browsers do rather than dropping it.

4. **Free space.** On each line the free space is distributed by the grow
   factors, or the deficit taken back by the shrink factors. Growing uses the raw
   factor; **shrinking is weighted by the factor times the base size**, so a large
   item gives up more than a small one — without that, a small item would shrink
   away to nothing. `min-width` and `max-width` are re-applied afterwards, which
   is what stops a shrunk item collapsing past its floor.

5. **Measure.** Each item is laid out at its resolved main size, which is what
   determines its cross size when that is not definite.

6. **Place.** `justify-content` distributes the leftover on the main axis,
   `align-items`/`align-self` position each item on the cross axis, and
   `align-content` spaces the lines themselves. The `gap` between items is part
   of the free-space calculation, so a row with gaps still fills exactly.

### The awkward parts

Three things are easy to get wrong, and each cost a bug here:

* **The main size must not be re-derived.** `layoutBlock()` resolves a box's
  width from its `width` property, so an item computed as 150px wide by flex
  would come out 10px wide when its style also said `width: 10px`. The item is
  laid out with a style whose width *is* the resolved main size, and the original
  style is put back afterwards. That style is switched to `content-box` for the
  duration, because flex works in content sizes while `box-sizing: border-box`
  describes the border box.

* **Stretch needs a definite size.** `align-items: stretch` has to give the item
  a height. An auto height would simply be re-measured from the content and come
  back unstretched, so the stretch is applied as an explicit size.

* **A flex container must not wrap its children in an anonymous block.** Every
  other block container does this, to keep block and inline content from mixing.
  For a flex container it is exactly wrong: the wrapper becomes a single flex
  item, so `justify-content` has nothing to distribute between. This is the bug
  that made a toolbar's two spans sit adjacent however `space-between` was set.

### Interaction with `box-sizing`

`box-sizing: border-box` is set by nearly every modern stylesheet, so it matters
that the engine honours it: a specified width or height includes the padding and
border, and they are subtracted to leave the content size the rest of layout works
in. Flex adds one rule on top — a border-box basis or width already includes the
padding, so it comes off before the item's base size is used for growing and
shrinking. Without that, a padded card overflows the row it is flexible in, which
is the most common flexbox pattern there is.

### Tests

`tests/unit/tst_layout.cpp` covers flexbox in twenty-five cases: direction and
reversal, grow, shrink, all six `justify-content` values, three `align-items`
values, stretch, columns, wrapping, gaps, `order`, `flex-basis` including
percentages, nesting, the anonymous-box rule, and `box-sizing` for both plain
boxes and flex items.

## Floats

A float is the one box that is simultaneously out of flow and part of it: it takes
no space in the block stack, yet the content beside it is shortened so the float is
not overlapped. That is the whole model, and it is why a float cannot be
implemented by simply skipping the box.

`FloatContext` holds the floats in force for one block formatting context as a list
of `FloatBand`s. A band is the float's *margin* box, because its margins are as
much of an obstacle as its border box, and bands are stored in absolute
coordinates so no translation is needed to compare them.

The context is deliberately not per-container. Floats belong to a formatting
context and escape their parent unless the parent establishes one, which is why a
float in one paragraph shifts the text of the next. `Context::floats` therefore
points at the nearest enclosing context, and only a box that establishes one gets a
fresh `FloatContext` of its own.

### Two passes, never both

A container either stacks block children or holds line boxes, and the tree builder
guarantees it is one or the other. `float` is handled in both passes, because a
float can be either level:

* **Block-level** floats are placed by `layoutBlockChildren()`, which calls
  `placeFloat()` at the current `y` and skips the box.
* **Inline-level** floats — the common case, a floated `<img>` or `<span>` — are
  taken out of the line by the collection pass in `layoutInlineRun()` and placed
  just before the line boxes are built, so the first line already knows they are
  there.

The distinction is load-bearing rather than tidy. A block-level float collected by
the inline pass as well is placed twice and lands shifted by its own width. The
guard is `!box->isBlockLevel()` in the inline branch.

### Placing one

`placeFloat()` follows CSS 2.2 §9.5:

1. **Width.** A float shrink-to-fits, so an auto width comes from
   `shrinkToFitWidth()`. A specified width wins outright — capping it by
   shrink-to-fit would collapse every `width: 120px` float holding no text to
   nothing, which is a bug the tests caught. Both branches are then reduced to a
   border-box width, clamped so the float can never be wider than its containing
   block leaves once its own margins are taken out.
2. **Vertical position.** The float drops until it fits beside the floats already
   in force. Each step moves to `nextBandY()`, the lowest bottom among the floats
   reaching the current `y` — not the lowest overall, or a float far below would
   drag ordinary content down with it.
3. **Horizontal position.** Pinned to the left or right edge of the room found,
   within the containing block.

### Avoiding one

`spanAt(context, y)` is what makes content flow around a float. It intersects two
rectangles: the *block's own content box*, and the room the formatting context
leaves at that `y`. Both halves matter. Using only the flow's edges made a
`width: 200px` paragraph break its lines against the viewport; using only the
block's own box would let text run through a float.

`layoutInlineRun()` calls it twice per line: once to decide where the line starts
and how narrow it is, and once per fragment to decide whether the next word fits.
`flushLine()` calls it again for the line it is about to place, so a line's origin
and width come from the span rather than from the container. When nothing fits
beside a float at all, the line moves to `nextBandY()` rather than breaking into
zero-width lines.

### `clear`

`clearY(y, clear)` moves a box below the floats the value names — `left`, `right`
or `both` — considering only floats that extend past `y`, since one already above
is already cleared. `clear` also ends margin collapsing for that box: the two boxes
are no longer adjacent, so their margins must not merge.

Note what `clear` does **not** do. It does not contain the floats of the element it
is set on; it moves that element past floats that precede it. The clearfix idiom
needs a generated `::after` box for exactly this reason, and
`tst_layout.cpp` says so in a comment rather than asserting the wrong thing.

### Containing them

A plain block does not contain its floats, so a parent whose only children are
floated collapses to zero height. That is not a bug to be fixed — it is the
behaviour that made clearfix necessary, and reproducing it is the point.
`aFloatDoesNotAddHeightWithoutAFormattingContext()` asserts the zero.

A box that *does* establish a formatting context grows to reach the lowest float
it holds, which is why `overflow: hidden` and `display: inline-block` are the two
ways to stop a container collapsing. `establishesFlow` names them: a flex
container, an inline-block, or a block whose `overflow` is not `visible`.

### Tests

`tst_layout.cpp` covers floats in twelve cases: both edges, shrink-to-fit, text
avoidance on the first line, a return to full width below the float, two left
floats side by side, a float dropping when there is no room, `clear` on a sibling,
containment by `overflow: hidden` and by `inline-block`, the deliberate absence of
containment without a formatting context, and a float staying inside a narrower
parent.

Four of those were written against a mutation to check they earn their place:
removing `clear`, removing float avoidance from `spanAt`, and removing
containment each make the relevant tests fail.

Two real bugs were found this way. The first was an infinite recursion: a float
re-appended to the wrong container became its own child, and layout descended into
it until the stack ran out — visible only as a `SIGSEGV` in a crash report. The
second was the double placement described above, which showed up as floats landing
at exactly one width too far right.

## Positioning

`position: relative`, `absolute`, `fixed` and `sticky` are implemented, along with
`top`/`right`/`bottom`/`left` and `z-index`. They split into two very different
mechanisms, and the split explains the shape of the code.

### In flow: `relative`

A relatively positioned box keeps the space it would have occupied; only its own
geometry moves. So `applyRelativeOffset()` runs at the *end* of the box's own
layout, after its height has been reported to its parent, and the value returned
to the parent is computed from the un-displaced position. Nothing above or beside
the box can tell that it moved, which is exactly the property authors rely on when
they nudge something without disturbing the page.

Only the offsets the author wrote displace it, and each axis is decided
separately: `left` wins over `right`, `top` over `bottom`, which is the CSS 2.2
rule for an over-constrained box.

### Out of flow: `absolute` and `fixed`

These are placed by a **second pass**, `layoutAbsoluteDescendants()`, which runs
after normal flow has finished. The ordering is forced by the specification: an
absolutely positioned box is placed against its containing block's padding box,
and `bottom: 0` needs that block's final height — which is only known once the
content inside it has flowed. The pass therefore runs depth-first: a box is placed
before its own positioned descendants, so a nested absolute box has a settled
ancestor to measure against.

An out-of-flow box takes no space. That single rule is enforced in five places,
and missing any one of them produces a visible bug:

- `layoutBlockChildren()` skips it, so it does not shift its siblings or take part
  in margin collapsing.
- `layoutInlineRun()` keeps it aside rather than measuring it into a line — and
  rather than destroying it when the line boxes replace the inline children.
- `shrinkToFitWidth()` ignores it, or a dropdown would widen the menu it hangs
  from.
- The content-height dispatch ignores it, or a container's only block-level child
  would stretch it.
- `collect()` in the inline measurement ignores its content, or its text would add
  a line to its container.

### `sticky`

Sticky positioning is the one feature whose answer depends on the scroll
position, which changes without a re-layout. Baking it into the box's geometry
would mean re-laying out the page on every scroll. It is therefore resolved while
**painting**, by `Painter::paintStickyBox()`: the box is translated for the
duration of its paint and moved back afterwards. Sticky boxes are few — a header,
a table heading — so the cost is not worth avoiding.

The box sticks within its containing block, so it scrolls away once its section
has gone past rather than floating forever. There is no nested scroll container in
this engine, so the viewport is always the constraint.

### `z-index`

Painting follows the CSS 2.2 layering rule reduced to what this engine models:
in-flow content first, then positioned boxes with a positive `z-index` on top of
it, lowest first. `Painter::paintChildrenInStackingOrder()` does that split. The
sort is stable so equal levels keep document order, which the specification
requires and which matters when a page relies on order between two boxes at the
same level.

A full implementation would give every stacking context its own layering and also
honour `opacity` and `transform`, neither of which this engine models.

### Two traps

* **Layout artifacts borrow their parent's style.** An anonymous block or a line
  box inside an absolutely positioned box reports that it is absolutely positioned
  too. Treating an artifact as out of flow makes layout place it, which gives it
  new artifact children, which are placed again — unbounded recursion.
  `Box::isOutOfFlow()` therefore checks the box's *type* as well as its style, and
  is used everywhere the question is asked.

* **A positioned inline keeps a box.** A plain `<span>` is dissolved into the line
  during inline layout, which leaves an absolutely positioned descendant with no
  containing block. `<span style="position:relative">` wrapping a dropdown is the
  pattern this exists for, so a positioned inline is built as an inline-block
  instead: atomic, so it does not break across lines.

### Size overrides need a stable home

Both flex items and absolutely positioned boxes are laid out at a size this engine
resolved rather than at the one their own `width` asks for, so they are given a
copy of their style with that size substituted. That copy cannot live on the
stack: layout stores style pointers in the boxes it builds — line boxes in
particular — and those are still read when the page is painted. `arenaStyle()`
keeps every override in a per-pass `std::deque`, whose elements have stable
addresses, and the arena is cleared at the start of the next layout.

### Tests

`tests/unit/tst_layout.cpp` covers positioning in ten cases: relative
displacement and its effect on flow, absolute placement against a positioned
ancestor, far-edge pinning, filling between opposite offsets, stretching between
`top` and `bottom`, nesting, out-of-flow space, content inside an absolute box,
sticky painting at two scroll offsets, and `z-index` paint order.

## Painting

`Painter` walks the box tree and draws boxes. It is stateless apart from its
`Options` and a `m_currentTextColor` field, which is how a nested element with no
colour of its own still paints in the inherited colour.

`Painter::Options` holds `defaultBackground` (white), `defaultTextColor` (black),
`scrollX`/`scrollY`, `showBoxModel`, `selection` (a rectangle in document
coordinates) and `selectionColor`. Two entry points: `paint(QPainter*, Box*)` for
a live widget and `renderToImage(root, width, height[, options])` for the
screenshot mode and the tests, which fills an `ARGB32_Premultiplied` image with
`canvasColor()` and enables antialiasing. `canvasColor(root)` returns the root
element's background colour, falling back to the `<body>`'s and then to
`defaultBackground` — the "canvas background" rule browsers implement, which is why
`<body style="background: blue">` colours the whole window.

### The draw order

`paintBox()` for a normal box does: **background → borders → children → overlay**.

* Anonymous and `Line` boxes draw only their children, because they have no
  appearance of their own. `Bullet` boxes go to `paintBullet()`; `Text` boxes to
  `paintText()`.
* `paintBackground()` fills the border box with `background-color`, using
  `drawRoundedRect()` when `border-radius` parses to a positive value (clamped to
  half the smaller side, so a large radius gives a pill). A `background-image`
  whose URL is a loaded image is drawn over the colour; `background-repeat`,
  `background-size` and `background-position` are stored but not honoured, so the
  image is stretched to the box.
* `paintBorders()` draws each side with a `QPen` of that side's width and colour.
  `none`, `hidden` and zero width are skipped. `dotted`/`dashed` use Qt line
  styles, `double` is drawn solid, and `groove`/`ridge`/`inset`/`outset` are
  approximated with a darker or lighter solid line. The sides are drawn as lines
  along their edges, not as mitred trapezoids, so differently coloured sides meet
  in a simple corner.
* `paintText()` sets the font from `LayoutEngine::fontFor()`, clips to the box, and
  draws at `top + ascent`. `underline` and `line-through` are explicit lines at
  metrics-derived positions; the selection rectangle, if any, is filled over the
  text with `selectionColor`.
* `paintBullet()` draws the marker just inside the list item's left padding,
  aligned with the first line's ascent with a fixed 7px gap, and skips markers more
  than 20px outside the clip rect.

`LayoutEngine::fontFor(style)` is the single place a `ComputedStyle` becomes a
`QFont`: point size from `font-size`, family list from `font-families` (with
`familiesFor()` mapping the generic families to concrete stacks, e.g. `monospace`
→ Menlo, Courier New), weight, italic, and letter/word spacing. The painter calls
it too, so measuring and drawing always agree.

### Culling and the box-model overlay

`paintBox()` reads `painter->clipBoundingRect()` and returns immediately when the
box is entirely outside it, with a 1px tolerance; an empty clip rect means no clip
is set, in which case nothing is culled.
`tests/unit/tst_layout.cpp::cullsOffscreenContent()` exercises this through
`renderToImage()`.

With `Options::showBoxModel` set (the toolbar's "Boxes" toggle in `ui::MainWindow`,
which calls `ui::PageView::setShowBoxModel()`), `paintBoxModelOverlay()` draws the
DevTools-coloured outlines on top of each box: content edge blue
`(0, 160, 255)`, padding edge green `(64, 190, 64)`, margin edge orange
`(230, 150, 0)`, all dashed. This is the first DevTools feature to reach the UI.

## Scrolling and hit testing in the UI

Scrolling is not part of the renderer: `Painter::viewportRect()` subtracts
`scrollX`/`scrollY` from each box's border box. `LayoutResult::documentHeight`
tells the view how far the page scrolls, computed as the deepest box bottom in the
tree (or the viewport height, whichever is larger), because a child can overflow a
parent with a specified height.

`LayoutResult::hitTest(x, y)` walks the line boxes and every descendant, keeping
the last box whose border box contains the point and whose width is not larger
than the current best — later visits are deeper, so the result is the most specific
box under the point. Anonymous and line boxes are excluded. `ui::PageView` uses its
own `elementAt()` walk instead, following the same children-first order, and then
looks up the ancestor chain for an `<a href>` in `linkElementAt()`, which is what
makes clicking text inside a link work.

## What a reader will notice

* Grid and tables do not affect layout. Flexbox, positioning and floats do.
* `documentWidth` is `max(viewportWidth, root right edge)`, so a page does not
  scroll horizontally unless a box overflows.
* Text is measured with platform fonts, so a layout can differ by a pixel or two
  between machines; the tests run with `QT_QPA_PLATFORM=offscreen` and assert
  ranges rather than exact widths.
* `Line` boxes are rebuilt from scratch every layout, because
  `Page::buildLayout()` reruns the whole pipeline when a late stylesheet or image
  arrives, and a document that nests deeper than 500 blocks is truncated with a
  warning.
