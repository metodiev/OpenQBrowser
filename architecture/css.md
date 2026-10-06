# CSS

`src/css/` implements enough of CSS to lay out real documents: a tokenizer
following CSS Syntax Level 3, a small value model, a selector matcher with
specificity, a forgiving stylesheet parser, media queries, and the cascade with
inheritance and presentational hints.

| File | Contents |
| --- | --- |
| `src/css/Tokenizer.h`, `Tokenizer.cpp` | `css::Token`, `css::TokenType`, `css::Tokenizer`. |
| `src/css/Value.h`, `Value.cpp` | `css::Value`, `values::parseColor()`, `values::lengthToPixels()`, `values::parseComponentValue()`, the named-colour table and `rgb()`/`hsl()` parsing. |
| `src/css/Selector.h`, `Selector.cpp` | `CompoundSelector`, `Selector`, `SelectorParser`, `matchesNth()`. |
| `src/css/Stylesheet.h`, `Stylesheet.cpp` | `Declaration`, `StyleRule`, `AtRule`, `Stylesheet`, `MediaQuery`. |
| `src/css/Grid.h`, `Grid.cpp` | `GridTrack`, `GridTrackList`, `parseTrackList()`, `GridPlacement`, `parsePlacement()`, `resolveTrackSizes()`. |
| `src/css/Style.h`, `Cascade.cpp` | `ComputedStyle`, `LengthOrAuto`, `EdgeSizes`, `StyleContext`, `StyleEngine`. |

## The tokenizer

`Tokenizer::nextToken()` produces tokens from `css::TokenType`: `Ident`,
`Function`, `AtKeyword`, `Hash`, `String`, `BadString`, `Url`, `BadUrl`, `Delim`,
`Number`, `Percentage`, `Dimension`, `Whitespace`, `Colon`, `Semicolon`, `Comma`,
brackets and braces, `Cdo`, `Cdc`, `EndOfFile`. Each `Token` carries the verbatim
`text`, the decoded `value`, and, where relevant, `number`, `unit`, `isId` and
`hasSign`. `Token::typeName()` gives the name used in error messages.

Supporting API: `peekToken()`, `pushBack()`, `consumeRawValue()`,
`skipWhitespace()` (which also swallows `/* … */` comments, including an
unterminated one at EOF), `position()`, `input()`, and
`readUntilMatchingBrace()` — used for at-rules and nested blocks the parser does
not understand, so their contents are skipped with balanced braces rather than
misparsed.

Escapes follow the spec: `\` plus up to six hex digits and one optional trailing
space becomes a code point, with `0`, out-of-range and surrogate values becoming
`U+FFFD`, and `\` plus any other character becoming that character. Negative
numbers and numbers with a leading `+` are produced by `wouldStartNumber()`,
which is what makes `-1px` a single `Dimension` token.

## The value model

`css::Value` is a tagged struct (`Kind`: `Invalid`, `Keyword`, `Length`,
`Percentage`, `Number`, `Color`, `Url`, `String`) rather than a variant, so
`ComputedStyle` can switch on the kind explicitly and a mistake is a compile
error. Besides the kind it carries `keyword`, `number`, `unit`, `color` and
`original` — the text the author wrote, which the inspector prints unchanged.

Lengths and percentages stay symbolic through parsing for a reason that
`makeLengthValue()` in `Stylesheet.cpp` explains: a relative unit such as `em`,
`rem`, `vw` or `vh` cannot be resolved without the element's font size and the
viewport, and resolving it early with a guessed font size would make
`font-size: 2em` wrong for every element that is not 16px. Absolute units
(`px`, `pt`, `pc`, `in`, `cm`, `mm`, `q`) are therefore converted to pixels
immediately by `values::lengthToPixels()`, while relative units keep their amount
and unit until the cascade resolves them. `em` uses the element's own font size,
`rem` the root font size, `vw`/`vh`/`vmin`/`vmax` the viewport, and `ex`/`ch` the
conventional half-em approximation.

`Value::toPixels(base, out)` deliberately refuses a unit other than `px` for a
`Length`, with the same reasoning in the comment: a relative unit is only
meaningful once the cascade knows the context. A unitless `0` is a valid length.

Colour parsing (`values::parseColor()`) accepts keywords, `#rgb`, `#rgba`,
`#rrggbb`, `#rrggbbaa`, and the functional notations `rgb()`, `rgba()`, `hsl()`,
`hsla()`, including the modern space-and-slash syntax. Function arguments that
cannot be parsed as numbers are reported through the returned `Value::Invalid`.

## Selectors and specificity

A `Selector` is a list of `Step`s, each a `CompoundSelector` plus the `Combinator`
that connects it to the compound on its right. Steps are stored **right to left**,
so `div.note > p em` is `[em] --Descendant--> [p] --Child--> [div.note]`, and the
subject of the selector is always `steps.first()`.

`CompoundSelector` holds a type name or the `universal` flag, a list of classes,
ids, `AttributeCondition`s (`Exists`, `Equals`, `Includes`, `DashMatch`, `Prefix`,
`Suffix`, `Substring`, with a `caseInsensitive` flag) and `PseudoClass`es.
`matches(element)` tests one compound against one element:
`first-child`, `last-child`, `only-child`, `root`, `empty`, the `-of-type`
family, `nth-child`/`nth-last-child`/`nth-of-type`/`nth-last-of-type` (via
`matchesNth()`, which evaluates An+B), `:not()`, `:is()`/`:where()`/`:matches`,
`link`/`any-link`. A compound that contains a pseudo-element never matches,
because `::before` and `::after` are parsed but not generated; an unknown
pseudo-class makes the compound fail, which is what the specification requires
for unsupported selectors.

`Selector::specificity()` packs the three parts defined in CSS Cascade §6.4.1 —
`specificityA` for ids, `specificityB` for classes, attributes and pseudo-classes,
`specificityC` for types and pseudo-elements — as `A * 10000 + B * 100 + C`, so a
single integer comparison orders two selectors.

`SelectorParser::parseList(text, unsupported)` splits on top-level commas
(`splitSelectorList()` ignores commas inside parentheses, brackets, strings and
escapes), parses each part, and appends unsupported parts to `unsupported`, which
the inspector and the stylesheet errors list show. `parse(text, error)` returns
an invalid `Selector` and fills `error` with `unsupported selector syntax near
"…"` for a single complex selector.

## Stylesheet parsing and error recovery

`Stylesheet::parse(source, origin, baseOrder, errors)` walks the token stream and
never throws:

* Stray `}`, `;`, `<!--` and `-->` (the HTML comment wrappers people put inside
  `<style>`) are skipped silently.
* An at-rule's prelude is read up to `{` or `;`. `@media` and `@supports` are
  parsed recursively by re-parsing the block text, and their rules are flattened
  with the media query attached to each rule. Other block at-rules
  (`@font-face`, `@keyframes`, …) are recorded in `atRules()` for the inspector
  and skipped.
* A style rule reads its selector list, then a declaration block.
  `readDeclarationBlock()` skips junk up to the next `;` or `}`, drops
  declarations whose value is missing or invalid (adding a message to `errors`
  when one is supplied), and skips nested blocks.
* A rule that ends up with no selectors or no declarations is not kept, because
  it could not contribute anything to the cascade.
* `parseDeclarationList(source, errors)` handles a `style="…"` attribute: a
  declaration list with no selector and no braces. A synthetic `}` is appended so
  an unterminated list still yields its declarations.

`tests/unit/tst_css.cpp::recoversFromMalformedInput()` pins the behaviour: given
`p { color: red }`, junk, `div { font-size: }`, `span { color: blue; }`, an
unknown at-rule and `a { color: green }`, the parser keeps exactly two usable
rules and reports the problems.

Shorthand expansion lives in the cascade, not the parser: `splitBoxValues()`
expands `margin`/`padding` by the one-to-four value rules, and the border
shorthand is scanned for a style keyword, a width and a colour in any order.
`extractImportant()` strips a trailing `!important` from a declaration's tokens.

## Media queries

`MediaQuery::matches(query, viewportWidth, viewportHeight, prefersDark)` evaluates
a comma-separated list with `and` conditions and `not` negation. A term whose type
is not `all`, `screen` or empty does not match, which is the conservative answer
for `print` and `speech`. Supported features: `width`, `height`, `min-width`,
`max-width`, `min-height`, `max-height` (values accept a bare number, `px`, `em`
or `%`), `orientation`, `prefers-color-scheme`, `prefers-reduced-motion`
(always `no-preference` because the browser draws statically), `hover` and
`pointer` (a desktop pointing device), and `min-resolution`/`max-resolution`
against an assumed 96 DPI.

Two details worth knowing:

* The cascade calls `Stylesheet::rulesForMedia(mediaType, viewportWidth)`, which
  passes **0** as the viewport height, so a height-based media query never matches
  during style computation even though `MediaQuery::matches()` can evaluate one.
  Width-based queries — the overwhelmingly common case — work.
* Any unknown feature makes the query fail to match, which is the safe default and
  is what the class documentation states.

### A media type can be joined to its conditions by `and`

`@media screen and (max-width: 1007px)` is the usual spelling, and the `and` is a
keyword rather than part of the type. Reading the type as `"screen and"` makes it
compare unequal to `screen`, and because an unrecognised type never matches, the
query silently fails. A stylesheet written entirely in that form then contributes
**no rules at all** — which is what happened to a real news site's responsive
grid, leaving it with no `grid-template-columns` and one column instead of four.
`parseMediaTerm()` therefore strips a trailing `and` from the type.

### A conditional rule is not later than an unconditional one

`Stylesheet` keeps plain rules and at-rule rules in separate lists, and
`rulesForMedia()` has to concatenate the matching ones. If it returns them
concatenated, every `@media` rule outranks every plain rule regardless of where it
was written, and the *narrowest* breakpoint in a stylesheet wins at every viewport
width. The method therefore restores source order with a stable sort on
`StyleRule::order`, which the parser assigns in document order.

Both of these are pinned by `tests/unit/tst_css.cpp`: `matchesAMediaTypeFollowedByAnd()`
and `keepsConditionalRulesInDocumentOrder()`. Each was checked by mutation —
putting the `and` back, and removing the sort, make the corresponding test fail.

## The cascade

`StyleEngine` is created with a `StyleContext` (viewport size, root font size,
`prefersDarkScheme`). The constructor prepends the browser's own stylesheet,
`StyleContext::userAgentStylesheet()`, an abbreviated CSS 2.2 Appendix D: block
display for the usual elements, `display: none` for `head`/`link`/`meta`/`style`/
`script`/`title`/`base`/`template`/`datalist`/`param`, `body { margin: 8px }`,
default margins and font sizes for headings, list markers, `table` display types,
`a:visited` colour, `[hidden] { display: none }`, and so on. `addStylesheet()`
appends author sheets in document order; `clearStylesheets()` restores just the
user agent sheet.

`computeStyles(document)` walks the tree iteratively — an explicit `QList<Frame>`
stack, not recursion — so a deeply nested document cannot exhaust the call stack,
and stores one `ComputedStyle` per element in `m_styles`.

`computeFor(element, parentStyle)` performs five steps:

1. **Inheritance first.** `inheritFrom()` copies the inherited properties
   (font family, size, weight, style, line height, text-align, transform,
   white-space, letter/word spacing, text-overflow, colour, visibility,
   list-style-type/position, border-collapse, caption-side, direction, cursor)
   from the parent so a declaration only has to override them.
   `style.fontSize` is set from the parent or the context's root size.
2. **Collect candidates.** Every matching declaration from every sheet, with its
   selector's specificity and the rule's `order`; `originRank` is `0` for the user
   agent sheet (`origin < 0`) and `1` for author sheets. Inline `style`
   declarations are collected too, with specificity `0`, `originRank` `2` and
   `order` `1000000`.
3. **Sort.** `std::stable_sort` orders by:
   `!important` first (with the origin reversal below), then `originRank`
   ascending, then specificity ascending, then order ascending. Applying the list
   in that order means the last and strongest declaration written actually wins.
4. **Apply.** `applyDeclaration()` handles each property. A declaration this
   engine cannot use is still recorded in `m_applied` with `used = false`, which
   is what `StyleEngine::declarationsFor()` and `Inspector::appliedRules()` show
   under "not applied". Properties the engine parses but does not act on
   (`box-shadow`, `transform`, `transition`, …) are marked unconsumed for that
   reason, as the comment at the end of `Cascade.cpp` explains. `StyleEngine::
   warnings()` is the matching diagnostic list; nothing appends to it today, so
   the inspector's warnings section is always empty and the "used = false" flag on
   a recorded declaration is what tells a reader a property was ignored.
5. **Presentational hints and defaults.** `applyPresentationalHints()` fills in
   properties the cascade left alone; then `lineHeight <= 0` becomes
   `fontSize * 1.2`; then the root element takes the window's base colours from
   `setBaseColors()` when nothing else set them.

### Origin order, importance and the reversal

The sort key combines importance and origin. Important declarations sort after
normal ones, so they are applied last and win. Among declarations of equal
importance the higher `originRank` sorts last — which is why an author rule beats
a user agent rule, and an inline `style` declaration (rank 2) beats both.

One detail is worth a careful reading of the comparator, because the comment above
it and its behaviour differ. The comment says "among them a lower origin wins,
which is the reversal CSS Cascade §6.4.4 describes", but the comparator sorts by
`a.originRank < b.originRank` regardless of importance, so among `!important`
declarations the **higher** origin is applied last and therefore wins. The
reversal is not implemented. It is unobservable in practice today because the
user agent sheet in `StyleContext::userAgentStylesheet()` contains no `!important`
declarations, so the two only ever meet on the author side.
`tests/unit/tst_css.cpp::appliesImportantAndOriginOrder()` covers the part that is
observable: `p { color: red !important }` beats `#x { color: blue }`.

### Specificity and document order

Within one origin and importance level the highest packed specificity wins, and
equal specificity is broken by `order`, which `Stylesheet::parse()` assigns from
its `baseOrder` argument and increments per rule. `Page::buildLayout()` relies on
this when it parses `<style>` elements in document order and re-adds cached
network stylesheets in request order.

### Inheritance and relative font sizes

`applyDeclaration()` receives `inheritedFontSize` separately from the style's
current font size. That is what makes nested `em` sizes compound correctly:
`font-size: 2em` is a multiple of the size the element would have had with no
font-size declaration at all, not of whatever another declaration already
applied. `font-size` accepts a percentage, a length (with `em` special-cased
against the inherited size), and the absolute and relative keywords from
`values::absoluteFontSizeKeyword()` and `values::relativeFontSizeKeyword()`.

### Presentational hints

`presentationalHints(element)` maps HTML attributes onto CSS declarations, as
HTML 4.01 Appendix A requires:

| Attribute | Property applied to |
| --- | --- |
| `align` | `text-align` on text elements; `float` or `vertical-align` on `img`, `table`, `iframe`, `input` |
| `color`, `face`, `size` | `color`, `font-family`, `font-size` on `<font>` |
| `bgcolor` | `background-color` |
| `background` | `background-image: url(…)` |
| `width`, `height` | `width`/`height` on `img`, `table`, `td`, `th`, `hr`, `iframe`, `embed`, `object`, `video`, `canvas`; a bare number means pixels |
| `border` | the four `border-*-width` plus `border-*-style: solid` on `table` and `img` |
| `cellpadding` | `padding` |
| `href` missing on `<a>` | `color: inherit; text-decoration: none` |

Hints are applied **after** the sorted author declarations and overwrite
whatever those left in the property. That is a known simplification: the standard
puts presentational hints between the user agent sheet and author rules, so an
author rule should be able to beat `bgcolor`. `tests/integration` and
`tests/unit/tst_css.cpp::appliesUserAgentDefaults()` cover the parts that are
stable.

### ComputedStyle

`ComputedStyle` in `src/css/Style.h` is an explicit struct rather than a property
map, for a reason its own comment gives: the layout engine reads the fields
directly, the inspector prints them without a lookup table, and a typo in a
property name is a compile error. It holds `display`, `position`, `visible`,
`isListItem`, the `LengthOrAuto` box properties (`width`, `height`, min/max,
margins, padding, offsets), per-side border style/width/colour, `borderRadius`,
`boxSizing`, `floatSide`, `clear`, `overflow`, `textAlign`, `verticalAlign`, the
typography properties, colours and background, list-style, `opacity`, and the
inherited set. Helpers: `generatesBox()`, `isBlockLevel()`, `isInlineLevel()`,
`isFloating()`, `isAbsolutelyPositioned()`, `describe()` (the one-line summary
the inspector prints).

`LengthOrAuto::resolve(base)` resolves a percentage against a base, returns the
pixel value for a length, and returns **zero** for `auto` — with the comment that
explains why: every caller that treats `auto` specially checks for it first, so a
stray `auto` here would silently produce a huge length.

## Which layout modes are unsupported

Parsed and stored, but with no effect in `src/renderer/`:

| Feature | What you observe |
| --- | --- |
| `display: flex`, `inline-flex` | Treated as a block formatting context; children stack vertically instead of flexing. |
| `display: grid` | Not a known `display` value, so the declaration is marked unused and the element keeps its previous display. |
| `display: table` and friends | Boxes are generated per the display type, but `LayoutEngine` has no table algorithm; rows and cells stack like blocks. |
| `float: left/right`, `clear` | Stored on the style and never read by `src/renderer/`; no float placement, no line shortening around floats. |
| `position: relative/absolute/fixed/sticky` | Stored; no offsets are applied, so a positioned box stays where static layout put it. |
| `vertical-align` | Stored on the style; inline layout always aligns on the baseline. |
| `text-align` | Stored; inline layout does not distribute space on the line. |
| `overflow` | Stored and never read by `src/renderer/`; no clipping and no scroll container. |
| `opacity` | Stored, clamped to 0..1; not applied when painting. |
| `box-shadow`, `transform`, `transition`, `animation` | Recorded as unused declarations so the inspector can show them. |
| `::before`, `::after` | Pseudo-elements are parsed into `pseudoElements` and never match, so no generated content. |
| `calc()`, custom properties, `@supports` conditions | `calc()` is not evaluated; `@supports` blocks are parsed but their condition is not tested. |
| Shorthand corners of `border-radius`, `border-spacing` | The radius takes only its first length (`parseRadius()` in `Painter.cpp`); `border-spacing` is never parsed into `ComputedStyle`. |
