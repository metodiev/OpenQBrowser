# HTML

`src/html/` turns bytes into a `dom::Document`. Three files do the work:

| File | Contents |
| --- | --- |
| `src/html/Tokenizer.h`, `Tokenizer.cpp` | `html::Tokenizer`, `html::Token` and `html::TokenAttribute`. |
| `src/html/Entities.h`, `Entities.cpp` | `entities::decode()`, the named-reference tables. |
| `src/html/Parser.h`, `Parser.cpp` | `html::Parser::parse()`, `parseFragment()`, and the internal `TreeBuilder`. |

The tokenizer is hand written and never throws; malformed input becomes text or a
comment and parsing continues. Comments in the source name the HTML standard
section each rule follows (`§13.2.5`, `§13.2.6.4.7`, `§13.1.2`, `§13.5`).

## Tokens

`html::Token` follows the standard's categories: `Character`, `Comment`,
`Doctype`, `StartTag`, `EndTag`, `EndOfFile`. Fields: `name` (tag name, comment
data or doctype name), `data` (character-token text), `attributes`,
`selfClosing` (the tag ended in `/>`) and `forceQuirks` (a doctype that cannot
enable standards mode). `isStartTag(tag)` and `isEndTag(tag)` are the helpers the
tree builder uses.

`Tokenizer::nextToken()` returns pushed-back tokens first, then delegates to
`readTextUntilEndTag()` when a text mode is active, otherwise to `readData()`.
`pushBack()` lets the parser look ahead without losing a token.

## Text modes

| Mode | Tags | Behaviour |
| --- | --- | --- |
| `Data` | everything else | Markup is recognised; character references are decoded by `readCharacterRun()`. |
| `RcData` | `title`, `textarea` | No tags are recognised; character references are still decoded. |
| `RawText` | `style`, `xmp`, `iframe`, `noembed`, `noframes`, `plaintext`, `noscript` | Text runs to the matching end tag; no reference decoding. |
| `ScriptData` | `script` | Same as raw text in this implementation. |

The mode is entered in `readStartTag()`: when the tag just read is one of the
above and the tag was not self-closing, `m_textMode` and `m_rawTextTag` are set.
`readTextUntilEndTag()` then scans for `</` + the tag name, case-insensitively,
at a tag-name boundary (`isTagNameTerminator()`: whitespace, `/` or `>`), emits
everything before it as one character token (decoding references only in RCDATA),
and then leaves the mode so the end tag is tokenized normally. If no end tag is
found, the rest of the input is text and the mode resets.

Deliberate deviations from the standard, both visible in the code:

* `plaintext` is treated like other raw text tags, so it ends at `</plaintext>`;
  the standard has it consume the rest of the file. A `</plaintext>` in a real
  document is rare, so the simpler rule is used.
* `ScriptData` does not implement the `<!--`/`-->` script-data escaping states.
  A `</script>` inside a JavaScript string will close the element, exactly as it
  would in a very early browser.

## Data-state tokenizing

`readData()` inspects the character after `<`:

* `<!--` starts a real comment (`readComment()`), which ends at the first `-->`
  or consumes the rest of the input.
* `<!` otherwise is a markup declaration: `<!DOCTYPE` (case-insensitive) goes to
  `readDoctype()`; anything else — including `<![CDATA[` — becomes a bogus
  comment up to `>`. (CDATA is only significant in foreign content, which is not
  implemented.)
* `</` followed by a letter is an end tag; `</>` is ignored via an empty comment
  token so the token stream stays aligned; `</` followed by anything else is a
  bogus comment.
* `<?` is a bogus comment, as the standard requires.
* `<` followed by a letter is a start tag.
* Any other `<` is emitted as a text character — the case that keeps the
  tokenizer making progress on input like `1 < 2`.

Tag names are lower-cased. `readAttributes()` skips whitespace, accepts a stray
`/` as ignorable, ends at `>` or `/>`, reads quoted and unquoted values, decodes
entities in attribute values, and keeps the **first** occurrence of a duplicated
attribute (standard §13.2.5.33). A tag left unterminated at end of input is
dropped rather than guessed at. `readComment()` and `readBogusComment()` both
handle an unterminated construct by consuming the remainder.

## Entity decoding

`entities::decode(text)` implements the character-reference rules of §13.5. It
returns the input unchanged when there is no `&`.

* Numeric references: `&#38;` and `&#x26;`. A missing digit run, `0`, a value
  above `0x10FFFF`, or a surrogate leaves the `&` literal; a valid value becomes
  the code point via `QString::fromUcs4`. A surrogate produce `U+FFFD`
  (`replacementCharacter()`); a terminated-but-invalid reference also becomes
  `U+FFFD`.
* Named references: the longest run of letters and digits after `&` is taken as
  the name. If it is followed by `;` and the name (plus `;`) is in `table()`, the
  value is substituted.
* **Legacy names without a semicolon.** `legacyTable()` holds exactly the names
  the standard lists as valid un-terminated: `amp`, `AMP`, `lt`, `LT`, `gt`,
  `GT`, `quot`, `QUOT`, `nbsp`, `copy`, `COPY`, `reg`, `REG`, `deg`, `trade`,
  `times`, `divide`, `plusmn`. They apply only when the following character is
  neither alphanumeric nor `=`, and a longest-prefix fallback handles cases like
  `&notin` decoding as `¬in`.
* Anything unmatched is emitted literally, semicolon included, which is what the
  standard requires for unknown names.

The table itself is a curated subset — Latin-1, punctuation, symbols, arrows,
mathematics and Greek — rather than all two thousand entries, chosen so the file
stays readable while covering the references real documents use. `table()` covers
about 230 names with the trailing semicolon included in the key.

## Tree construction

`Parser::parse(html, url, options)` creates a `dom::Document`, runs the internal
`TreeBuilder`, then guarantees the skeleton. `ParseOptions::maxDepth` defaults to
400.

`TreeBuilder` holds the two structures the standard describes: a stack of open
elements (`m_openElements`) and pointers to `m_html`, `m_head`, `m_body`, plus
pending text, warnings and the doctype flag. `process()` dispatches on the token
type.

**Implied elements.** Nothing is created until needed:

* `ensureHtmlElement()` creates `<html>` and installs it with
  `Document::setDocumentElement()`.
* `ensureHeadElement()` creates `<head>` inside `<html>`.
* `ensureBodyElement()` creates `<body>`, appends it to `<html>` and pushes it
  onto the open-element stack.
* `startBody()` drops the open-element stack back to `<html>` and creates
  `<body>` — this is what ends the head when content appears.
* After the walk, `Parser::parse()` creates whatever is still missing, inserting
  `<head>` before an existing `<body>`.

**In-head handling.** `isHeadElement()` covers `base`, `basefont`, `bgsound`,
`link`, `meta`, `title`, `noscript`, `noframes`, `style`, `template`, `script`.
While `inHead()` is true, such a tag is placed in `<head>`; void head elements
(`link`, `meta`, …) are not pushed as open elements, while `title`, `style` and
`script` are, so the tokenizer's raw text mode delivers their content into the
right element. A second `<html>` merges its attributes into the existing element;
a second `<head>` is ignored once the body has started.

What the stack looks like while `<p>one<p>two` is parsed, with the second `<p>`
auto-closing the first:

```
  token            open elements (bottom -> top)      action
  <p>              html, body, p                      push p
  "one"            html, body, p                      text into p
  <p>              html, body                         closeOpenElement("p")
  <p>              html, body, p                      push the new p
  "two"            html, body, p                      text into the new p
```

**Text handling.** Adjacent character tokens are buffered in `m_pendingText` so
they become one text node, flushed before any structural token. Text is discarded
when it sits in the head slot and is whitespace only, and when it is whitespace
between block siblings in `<body>`. Text that is neither starts the body
(§13.2.6.4.4), which is why a bare `hello` in a document still renders.

**Auto-closing.** `processStartTag()` applies these rules, in this order:

| Starting tag | Closes |
| --- | --- |
| any tag in `closesParagraph()` (`address`, `article`, `aside`, `blockquote`, `details`, `div`, `dl`, `fieldset`, `figcaption`, `figure`, `footer`, `form`, `h1`–`h6`, `header`, `hgroup`, `hr`, `main`, `menu`, `nav`, `ol`, `p`, `pre`, `search`, `section`, `table`, `ul`) | an open `p` |
| `li` | an open `li` |
| `dd`, `dt` | an open `dd` and an open `dt` |
| `tr`, `tbody`, `thead`, `tfoot` | an open `td` and `th` |
| `td`, `th` | an open `td` and `th` |
| `h1`–`h6` | any open heading |

`closeOpenElement(tag)` finds the nearest matching element on the stack and pops
through it, which implicitly closes anything left open above it.

**Void elements.** `isVoidElement()` lists the standard set (`area`, `base`,
`br`, `col`, `embed`, `hr`, `img`, `input`, `link`, `meta`, `param`, `source`,
`track`, `wbr`, `basefont`, `bgsound`, `frame`, `keygen`). Void elements and tags
written with `/>` are never pushed as open elements. On non-void HTML elements
the `/` is ignored, as browsers do.

**Table-only elements.** `isTableOnly()` — `caption`, `col`, `colgroup`, `tbody`,
`td`, `tfoot`, `th`, `thead`, `tr` — are dropped with a warning when they appear
outside a table context, instead of being foster-parented.

**End tags.** `processEndTag()` searches the stack for the nearest element with
that name and pops through it, closing anything above implicitly. An unmatched
`</x>` becomes a warning: `"Stray </x> tag ignored"` for known names,
`"Unknown end tag </x> ignored"` otherwise. `</head>` closes the head element;
`</body>` and `</html>` mark the body closed and are otherwise no-ops.

**Depth limit.** A start tag beyond `ParseOptions::maxDepth` (400) is rejected
with `"Maximum element depth reached at <x>"`, which keeps hostile input from
producing a tree that later recursive walks cannot handle.

**Quirks mode.** `processDoctype()` sets `Document::setQuirksMode()` when
`forceQuirks` is set or the doctype name is not `html`. `readDoctype()` marks
`forceQuirks` when the name is not `html` or when the declaration contains
`PUBLIC` or `SYSTEM` — so only a bare `<!DOCTYPE html>` gives standards mode. No
doctype at all sets quirks mode in `TreeBuilder::run()` and adds the warning
`"Missing doctype; document parsed in quirks mode"`.

`Document::quirksMode()` is recorded on the document and reported by tests, but
**no CSS or layout rule currently consults it**. It is a deliberate placeholder.

## Warnings and fragments

`ParseResult` carries `tokenCount` and `warnings`, and `Parser::parse()` puts the
document in a `std::unique_ptr<dom::Document>`. `Parser::parseFragment(html,
document, contextTag)` parses standalone markup and returns the created nodes,
moving them out of a container chosen from the context tag: `table`, `tr`,
`tbody`, `thead`, `tfoot` use the parsed table element, `head` uses the head,
everything else falls back to `<body>`. That is enough for innerHTML-style use
and for tests, and it is not a full fragment algorithm.

## What a reader will notice

* Misnested formatting tags are not repaired. `<b><i></b></i>` produces the tree
  the open-element stack implies, because there is no adoption agency algorithm.
* `<table>` content is not foster-parented; stray table elements are dropped with
  a warning instead of being moved before the table.
* `<template>` is parsed as an ordinary element; its content is not inert.
* No foreign content: `<svg>` and `<math>` become ordinary HTML elements with no
  namespace, so SVG markup is not parsed with SVG's own content model.
* No character-encoding sniffing beyond `HttpResponse::text()`'s `<meta charset>`
  rule and the `Content-Type` header; the tokenizer only ever sees a `QString`.
* `noscript` is tokenized as raw text, so markup inside it is not parsed even
  when scripting is off.
* Tokenizer positions (`Tokenizer::position()`) are available but no parse errors
  with line numbers are produced.
