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
to `context.availableWidth`. It is used for auto-width inline-blocks; floats never
call it because floats are not placed. After the clamp, the box is laid out as a
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
and never read by `src/renderer/`.

| Property | What you observe |
| --- | --- |
| `float`, `clear` | Nothing floats; lines are not shortened around a float. |
| `position`, `top`/`right`/`bottom`/`left` | A `position: relative; left: 30px` box stays exactly where block layout put it. |
| `vertical-align` | Lines are always baseline-aligned. |
| `text-align` | A line always starts at the containing block's left content edge. |
| `overflow`, `display: flex` | Nothing clips and no new formatting context is established. |
| `display: table*` | Rows and cells stack as ordinary blocks at full width. |

A reader should expect these features to have **no layout effect** rather than to
produce a warning of their own. Note that the class comment in `Layout.h` still
lists "floats" and "table display types" among the features the engine covers;
that sentence overstates the code, and this table is the accurate statement.

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

* Floats, positioning, flex, grid and tables do not affect layout.
* `documentWidth` is `max(viewportWidth, root right edge)`, so a page does not
  scroll horizontally unless a box overflows.
* Text is measured with platform fonts, so a layout can differ by a pixel or two
  between machines; the tests run with `QT_QPA_PLATFORM=offscreen` and assert
  ranges rather than exact widths.
* `Line` boxes are rebuilt from scratch every layout, because
  `Page::buildLayout()` reruns the whole pipeline when a late stylesheet or image
  arrives, and a document that nests deeper than 500 blocks is truncated with a
  warning.
