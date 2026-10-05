# Contributing

OpenQBrowser is developed in a single repository with one build and one set of
tests. This document describes the workflow the tree actually assumes: how to add
a file, where code and comments belong, and what a change is expected to bring with
it.

## Workflow

```bash
git clone <repository> && cd OpenQBrowser

cmake --preset default
cmake --build build -j
ctest --test-dir build --output-on-failure

# Make a change, then:
cmake --build build -j && ctest --test-dir build --output-on-failure
```

The loop is short on purpose: a full build of this tree takes well under a minute
on a laptop, and the whole test set runs in a few seconds, so there is no reason to
batch changes. On macOS, if the link step fails with `tapi error: malformed file`
from a CommandLineTools SDK, configure with
`-DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)"` — `build.md` explains
the whole issue.

Handy while working on a stage:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-all about:home
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-layout /dev/stdout page.html
QT_QPA_PLATFORM=offscreen ./build/bin/tst_css -functions          # list cases
QT_QPA_PLATFORM=offscreen ./build/bin/tst_css appliesSpecificity   # run one case
```

`--dump-dom`, `--dump-layout`, `--dump-boxes`, `--dump-styles` and `--dump-all`
print exactly the state the tests assert on, and `--screenshot=FILE` renders the
same image the painter would.

## Module layout

`src/CMakeLists.txt` opens with the layout and the rule that governs it:

```
network/     URLs, HTTP/1.1, HTTPS, redirects and resource loading
dom/         Document Object Model
html/        HTML tokenizer and tree construction
css/         CSS tokenizer, parser, selectors and the cascade
renderer/    Box tree, layout, painting and image decoding
storage/     History, bookmarks and cookies
security/    Origin, permission and TLS policy
javascript/  The JavaScript integration seam (see architecture/javascript.md)
devtools/    Page inspector
browser/     Page and Tab orchestration, built-in pages
ui/          Qt Widgets shell: window, tabs, address bar
```

Each directory owns one stage and **depends only on the stages before it**:

```
network -> dom -> html -> css -> renderer -> browser -> ui
```

A new file belongs in the directory of the stage it serves, and should include
only earlier stages. If a change seems to need a later stage, the change is
probably in the wrong directory, or the piece that is missing belongs in the later
stage and should be called from there. `overview.md` lists the public header of
each module; a header is public when another module includes it, and it should
stay small.

Includes are always written from the `src` root — `#include "network/Url.h"`,
`#include "css/Style.h"` — because `oqb_project_options` puts `src` on the include
path. Never use a relative `../` path.

## A new source file must be added to `src/CMakeLists.txt`

This is the rule most likely to trip up a new contributor, because a file that is
not listed simply is not compiled, and the failure looks like a missing symbol at
link time rather than a build error.

```cmake
add_library(oqb_core STATIC
    network/Url.cpp
    ...
    devtools/Inspector.cpp
    browser/BuiltinPages.cpp
    browser/Page.cpp
    browser/Tab.cpp
)

add_library(oqb_ui STATIC
    ui/PageView.cpp
    ui/MainWindow.cpp
)
```

Put a file in `oqb_core` unless it uses Qt Widgets; anything that includes a
`QWidget`, `QMainWindow`, `QLineEdit` and so on goes in `oqb_ui` — that split is
what keeps the headless modes and the unit and integration tests free of a
windowing dependency (`build.md`). A new test file is added the same way, to
`tests/unit/`, `tests/integration/` or `tests/browser/`, with one extra line:
`oqb_add_unit_test(tst_foo)` (or the integration/browser variant) after creating
`tests/<suite>/tst_foo.cpp`.

`CMakeLists.txt` files are the only build description; there is no Makefile, no
`.pro` file and no generated build script checked in.

## Coding style in the tree

The style is consistent across `src/` and worth matching exactly, because the
consistency is what makes the tree readable.

### Naming

* Types are `PascalCase`: `Url`, `HeaderList`, `HttpClient`, `ResourceLoader`,
  `Tokenizer`, `TreeBuilder`, `StyleEngine`, `ComputedStyle`, `LayoutEngine`,
  `Painter`, `HistoryStore`, `SecurityPolicy`, `MainWindow`.
* Functions and variables are `camelCase`: `effectivePort()`, `toRequestTarget()`,
  `removeDotSegments()`, `decodeContentEncoding()`, `buildLayout()`,
  `collapsedMargin`, `availableWidth`, `lineAscent`.
* Member variables are `m_camelCase`: `m_buffer`, `m_expectedBody`, `m_openElements`,
  `m_viewportWidth`, `m_parseResult`. A file-local singleton-style pointer uses the
  `g_` prefix (`g_styleEngine` in `BoxTree.cpp`).
* Static and file-local constants are `kPascalCase`: `kMaxBodyBytes`,
  `kMaxEntries`, `kMaxSubresources`, `kKnown`, `kTableOnly`, `kNumerals`.
* Namespaces are `oqb` plus the module: `oqb::network`, `oqb::dom`, `oqb::html`,
  `oqb::css`, `oqb::renderer`, `oqb::storage`, `oqb::security`, `oqb::javascript`,
  `oqb::devtools`, `oqb::browser`, `oqb::ui`. Declared as `namespace oqb::network {`
  and closed with `} // namespace oqb::network`.
* Enumerators are `PascalCase` inside a scoped enum: `Url` has none, but
  `HttpRequest::Method::Get`, `HttpClient::State::Connecting`,
  `Tokenizer::TextMode::RcData`, `Box::Type::InlineBlock` and
  `Value::Kind::Percentage` are the pattern.
* Headers use `#pragma once`, never include guards.
* Private helpers that are not part of a class go in an anonymous `namespace { … }`
  at the top of the `.cpp` (see `Url.cpp`, `HttpClient.cpp`, `Layout.cpp`,
  `BuiltinPages.cpp`), closed with `} // namespace`.

### Formatting

* Four-space indent, no tabs.
* Braces on the same line for functions, classes, `if`, `for` and `while`.
* No braces for a single-statement body (the tree uses this freely, e.g.
  `if (segment.isEmpty()) …` and early `return` guards).
* Roughly 96–100 columns; long argument lists are wrapped and aligned, as in
  `applyDeclaration(ComputedStyle *style, const Declaration &declaration, …)`.
* The compiler enforces `-Wall -Wextra -Wpedantic`; a clean build is the
  expectation, and `OPENQBROWSER_WARNINGS_AS_ERRORS=ON` turns warnings into errors
  for anyone who wants that locally in CI.
* `Q_UNUSED(x)` is used deliberately where a parameter exists for a future purpose
  or to keep an interface stable (`Q_UNUSED(cellSpacing)` in `Cascade.cpp`,
  `Q_UNUSED(mediaType)` in `Stylesheet.cpp`) — but prefer removing an unused
  parameter over adding the macro.

### Comments: what they explain, and where they go

The tree's comments are its documentation, and they have a specific job: they
explain the *why* behind a decision, a rule's origin, or a non-obvious consequence.
They are not narration of what the next line does.

* **Public API documentation** is a `///` doc comment on the declaration in the
  header, not in the `.cpp`. `ResourceLoader.h`, `HttpClient.h`, `Style.h` and
  `BoxTree.h` are good examples. A class comment states the contract and the
  invariants (for example `HttpClient`'s "One client instance performs one request
  chain and then emits exactly one of finished() or failed(). Instances are cheap").
* **Rule citations.** Where behaviour comes from a specification, the section is
  named — `/// RFC 3986 section 5.2.4, "Remove Dot Segments"`,
  `/// §13.2.6.4.7`, `// CSS 2.2 §10.3.2`, `// CSS Cascade §6.4.1`. This is the
  single most common comment in the tree and the most useful one to keep up to
  date.
* **Non-obvious reasoning.** Three examples worth reading before writing a new
  comment: `Url.cpp`'s explanation of why a trailing `..` leaves a slash;
  `Layout.cpp`'s explanation of why the measured inline children are released
  before the line boxes are built; `Cascade.cpp`'s explanation of why relative units
  are not resolved while parsing. Each says what would go wrong otherwise.
* **Trap warnings.** `Value::toPixels()` explains why a unit other than `px` is
  refused; `LengthOrAuto::resolve()` explains why `auto` resolves to zero;
  `BoxTree.cpp` explains why the style engine must outlive the box tree. These
  comments exist because the alternative is a subtle bug.
* **Section banners** in larger files are `// ------------ name -------------`
  lines separating the parts of a file (`HttpMessage.cpp`, `Cascade.cpp`,
  `Layout.cpp`, `Painter.cpp`).
* Do not write comments that restate the code, do not leave commented-out code,
  and do not use `TODO` as a substitute for saying what is missing and why.
* Keep the writing plain: complete sentences, no marketing adjectives, no emoji.
  The comment style in `src/` is the reference.

### Dependencies and Qt

* Prefer Qt's containers and strings (`QString`, `QList`, `QHash`, `QPair`) in the
  modules that already use them, and `std::vector`/`std::unique_ptr` where
  ownership is the point (`dom::Node::m_children`, `renderer::Box::m_children`) —
  the comment at `dom/Node.h` records that reason.
* Owning pointers are `std::unique_ptr`; non-owning pointers are raw and their
  lifetime rule is documented at the declaration (`Box`'s style pointer,
  `Page`'s history and bookmark pointers).
* New Qt classes that need signals or `tr()` declare `Q_OBJECT`; `CMAKE_AUTOMOC` is
  on, so no extra build step is required.
* Do not add a dependency to `oqb_core` without checking that the headless build
  still links; `Qt6::Widgets` must not appear in `oqb_core`.

## Changes come with tests

A change is expected to come with a test in the suite that owns the behaviour:

| Change | Suite |
| --- | --- |
| URL parsing, resolution, origins, user input | `tests/unit/tst_url.cpp` |
| Tokenizer, entities, tree construction, quirks mode | `tests/unit/tst_html.cpp` |
| CSS tokens, values, selectors, media queries, cascade | `tests/unit/tst_css.cpp` |
| Box tree, geometry, painting | `tests/unit/tst_layout.cpp` |
| HTTP, redirects, encodings, loading, cache | `tests/integration/tst_http.cpp` |
| Redirect targets, late stylesheets, large real pages | `tests/integration/tst_redirect.cpp` |
| Several stages together | `tests/integration/tst_pipeline.cpp` |
| Window, tabs, address bar, scrolling, link clicks | `tests/browser/tst_browser.cpp` |

`testing.md` lists what each suite covers today and describes the practice the
project follows: write the failing case first, fix the component rather than the
test, keep the case afterwards, and assert arithmetic or pixel *predicates* rather
than golden images, because metrics come from the machine's fonts. Every suite runs
with `QT_QPA_PLATFORM=offscreen`, which the CMake helpers set, so a test binary run
by hand needs the variable exported.

If a change cannot be tested at the level above, say so in the pull request and
explain what was verified instead — a dump from `--dump-all`, a screenshot, or a
manual run — rather than leaving the behaviour unverified.

## Adding a whole feature

The order that follows from the dependency rule, and the order the existing
modules were built in:

1. The owning stage first: the type and its test (for example a new property in
   `ComputedStyle` plus a cascade test in `tst_css.cpp`).
2. The consumer second, with its own test (layout honouring the property, in
   `tst_layout.cpp`).
3. The end-to-end suite last, so the feature is exercised the way a page uses it
   (`tst_pipeline.cpp` or `tst_browser.cpp`).
4. Documentation: the module document in `architecture/` that describes the stage
   should be updated in the same change, because these documents are written
   against the source rather than against a plan.

Do not leave a stage half-wired: an option that is parsed but never consulted, or a
policy that is implemented but never called, should either be finished or be
described honestly as not yet wired — the documents in `architecture/` keep that
distinction explicit, and `security.md` and `rendering.md` show the form.
