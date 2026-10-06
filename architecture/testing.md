# Testing

OpenQBrowser has three test suites — unit, integration and browser — containing
twelve QTest-based targets, all registered with CTest from `tests/CMakeLists.txt`:

| Suite | Directory | Targets | Contents |
| --- | --- | --- | --- |
| Unit | `tests/unit/` | `tst_url`, `tst_html`, `tst_css`, `tst_layout`, `tst_javascript` | One component each, in isolation; `tst_javascript` runs real scripts against a real parsed document. |
| Integration | `tests/integration/` | `tst_http`, `tst_pipeline`, `tst_redirect`, `tst_perf`, `tst_scripting` | Several components together, including real loopback HTTP; `tst_scripting` drives scripted pages through the load pipeline. |
| Browser | `tests/browser/` | `tst_browser`, `tst_devtools` | The window, its tabs and user-level navigation, and the developer tools panel. |

Every target links `oqb_core` (and `tst_browser` additionally links `oqb_ui` and
`Qt6::Widgets`), is built as a plain console executable
(`MACOSX_BUNDLE OFF`, `WIN32_EXECUTABLE OFF`) and runs with
`QT_QPA_PLATFORM=offscreen`, which the CMake files set as a test property rather
than leaving to the caller.

```
tests/
  CMakeLists.txt
  unit/          CMakeLists.txt, tst_url.cpp, tst_html.cpp, tst_css.cpp, tst_layout.cpp
  integration/   CMakeLists.txt, tst_http.cpp, tst_pipeline.cpp,
                 tst_redirect.cpp, tst_perf.cpp
  browser/       CMakeLists.txt, tst_browser.cpp
  fixtures/      (empty; reserved for test data)
```

## Unit suite

### `tests/unit/tst_url.cpp`

Covers `oqb::network::Url`. `parsesAbsoluteHttp`, `parsesPorts`,
`normalizesDefaultPorts`, `parsesQueryAndFragment`, `rejectsEmptyHost`,
`resolvesRelativeReferences` (a data-driven table of base/reference/expected),
`resolvesDotSegments` (another table, pinning the trailing-slash rule for `/a/b/..`),
`buildsOrigins`, `handlesFileUrls`, `interpretsUserInput` (`localhost:8080/x` must
not parse as a scheme, and a phrase must be rejected so the caller searches),
`buildsSearchQueries`, `extractsAboutPage`, `understandsIpv6Hosts` and
`detectsSchemes`.

### `tests/unit/tst_html.cpp`

Covers the tokenizer, `entities::decode()` and tree construction.
`decodesNamedEntities`, `decodesNumericEntities`, `leavesUnknownEntitiesAlone`,
`decodesLegacyEntitiesWithoutSemicolon`, `tokenizesStartAndEndTags`,
`tokenizesAttributes`, `handlesQuotedAndUnquotedAttributes`,
`keepsFirstDuplicateAttribute`, `treatsScriptContentAsText`,
`treatsStyleContentAsText`, `readsComments`, `buildsElementTree`,
`createsImpliedElements`, `autoClosesParagraphs`, `autoClosesListItems`,
`autoClosesTableCells`, `handlesUnclosedTags`, `ignoresStrayEndTags`,
`setsQuirksMode` (asserts standards mode for `<!DOCTYPE html>`, quirks mode for no
doctype and for an HTML 4.01 `PUBLIC` doctype), `extractsTitle`,
`extractsMetadata`, `serializesBackToHtml` and `resolvesUrlsAgainstBase`.

### `tests/unit/tst_css.cpp`

The largest unit file. Tokenizer tests (`tokenizesIdentifiersAndNumbers`,
`tokenizesStringsAndUrls`, `tokenizesSymbols`, `ignoresComments`), colour parsing
(`parsesHexColors` is data-driven, `parsesColorFunctions`, `parsesNamedColors`),
`parsesLengthUnits` (absolute units to pixels, relative units left symbolic),
selectors (`parsesSelectorLists`, `computesSpecificity`, `matchesCombinators`,
`matchesAttributeSelectors`, `matchesPseudoClasses`,
`splitsSelectorListsOnTopLevelCommas`), the parser (`parsesRulesAndDeclarations`,
`parsesImportantDeclarations`, `parsesShorthands`, `recoversFromMalformedInput`),
media queries (`parsesMediaQueries`, `evaluatesMediaQueries`) and the cascade
(`appliesInheritance`, `appliesSpecificity`, `appliesImportantAndOriginOrder`,
`appliesInlineStyles`, `computesRelativeFontSizes`, `appliesUserAgentDefaults`).

### `tests/unit/tst_layout.cpp`

Covers box tree construction, layout and painting through a small `Page` fixture
that parses, styles, builds boxes and lays out at a given viewport and keeps every
intermediate object alive, with `boxForId()`/`boxForElement()` helpers. Cases:
`buildsBoxesForElements`, `skipsDisplayNone`, `createsAnonymousBlocks`,
`laysOutBlockStack`, `appliesPaddingAndBorder`, `respectsSpecifiedWidth`,
`resolvesPercentageWidth`, `centresWithAutoMargins` (asserts the exact arithmetic
of `margin: 0 auto` inside `body`'s 8px margin), `collapsesAdjacentMargins`,
`wrapsLongTextIntoLines`, `laysOutInlineElements`, `honoursLineHeight`,
`positionsListMarkers`, `sizesReplacedElements`, `growsDocumentHeightWithContent`,
`paintsBackgroundColours`, `paintsTextPixels` and `cullsOffscreenContent`.

## Integration suite

### `tests/integration/tst_http.cpp`

Covers `HttpClient`, `ResourceLoader` and the `http::` helpers against a real
loopback socket. The whole file is driven by `TestServer`, a `QTcpServer` subclass
that

* listens on `QHostAddress::LocalHost` with port 0, so the kernel chooses a free
  port and tests never collide;
* hands out URLs with `urlFor(path)` as `http://127.0.0.1:<port><path>`;
* serves canned responses queued with `enqueueResponse()`, records every raw
  request in `receivedRequests()`, and closes the connection after writing by
  default, which is what makes the read-until-close framing path finish.

`tst_redirect`'s `externalStylesheetMakesThePageRenderable` builds its own server
the same way for the browser-level case. This is the loopback approach: no test in
`tst_http` touches the internet, which is why the suite is fast and repeatable and
why its 120-second CTest timeout is generous rather than necessary.

Cases: `fetchesSimpleResponse`, `sendsCorrectRequestLine`,
`sendsHostHeaderWithPort`, `sendsDefaultUserAgent`, `readsChunkedResponse`,
`decodesGzipResponse`, `handlesReadUntilClose`, `followsRedirects`,
`followsPermanentRedirect`, `stopsAfterTooManyRedirects`, `reportsHttpErrors`,
`reportsConnectionFailure`, `timesOutSlowServer`, `rejectsWrongContentLength`,
`loaderCachesResults`, `loaderAppliesConcurrencyLimit` (three fetches with a limit
of one must all complete, proving the queue drains), `loaderFetchesLocalFiles`
(using `QTemporaryDir`), `decodesChunkedEncoding` and `decodesGzipEncoding`.

### `tests/integration/tst_perf.cpp`

Guards against layout behaving worse than linearly in the size of a document.
These are not micro-benchmarks; they exist because a real article page took
minutes to load, and the cause was a layout pass per arriving image rather than
one coalesced pass. The cases are a flat document, many inline-blocks, nested
inline-blocks, and a comparison of 300 against 2400 elements that fails if the
cost grows dramatically faster than the element count.

### `tests/integration/tst_redirect.cpp`

Whole-browser cases for the paths where the network layer hands control back to
the browser, plus one case that needs no socket.

* `relativeRedirectIsResolvedAgainstTheDocument` — no socket. Builds an
  `HttpResponse` by hand with `statusCode = 302` and a `finalUrl` of
  `https://a.test/dir/page`, then asserts `HttpResponse::location()` for a
  root-relative `Location` (`/elsewhere`) and a path-relative one (`next` → the
  base's directory is kept).
* `externalStylesheetMakesThePageRenderable` — runs its own `QTcpServer` on
  loopback, serving an HTML document that links `/s.css` and the stylesheet
  itself. It waits for `Page::finished` and asserts that
  `StyleEngine::styleFor(body).backgroundColor` is the colour from the fetched
  stylesheet, which is what proves the second phase of the load re-runs the
  cascade and re-lays out without losing the box tree.
* `redirectChainEndsAtTheFinalUrl` — loads `http://github.com` and asserts that
  `Page::finalUrl()` ends up on `https` when the load did not fail.
* `survivesALargeRealPage` — loads a large real page
  (`https://en.wikipedia.org/wiki/Web_browser`) with images and an external
  stylesheet, and asserts the document and box tree exist with a node count above
  100. Its comment records why it exists: this is the shape of document that
  exposed a crash in the re-layout path.
* `survivesDeeplyNestedMarkup` — parses, styles, boxes and lays out documents
  nested 10, 50, 120 and 200 blocks deep, checking that each produces a document
  height. It builds the document directly rather than loading it, so it is
  independent of the network and exercises the `layoutBlock` ↔ `layoutInlineRun`
  recursion.

The last two cases reach the public internet and assert only guarded properties,
so a network failure is tolerated; a crash is not. A crash in this suite means a
real bug in the re-layout or recursion path, not a flaky test.

### `tests/integration/tst_pipeline.cpp`

Covers parse → style → box tree → layout → paint as one chain, with a `Rendered`
fixture that keeps every stage's output and an image rendered at the requested
viewport, plus a `countPixels()` template used to assert what was drawn. Cases:
`rendersStyledDocument`, `appliesExternalAndInlineStylesTogether`,
`rendersNestedLayoutCorrectly`, `escapesAndRendersEntities`,
`handlesRealisticPage`, `scalesWithViewport`, `rendersManyElements` and
`survivesMalformedMarkup`. This is the suite that catches a regression in how the
stages fit together rather than in one stage.

## Browser suite

`tests/browser/tst_browser.cpp` drives the browser the way a user does, through
`ui::MainWindow`, `ui::PageView` and `browser::Tab`. It uses a `waitForLoad()`
helper that spins `QCoreApplication::processEvents()` until the tab reports
`loadFinished`/`loadFailed` or the timeout expires, and a real window built with
`window.grab()` for rendering assertions. Cases: `loadsAboutPageIntoWindow`,
`rendersTheWindow` (counts light and dark pixels in the grabbed frame),
`tabNavigationRecordsHistory`, `backAndForwardMoveThroughHistory`,
`newTabKeepsItsOwnPage`, `addressBarInputBecomesSearch`,
`errorPageIsShownForAFailedLoad`, `pageViewScrollsWithinTheDocument`,
`pageViewFindsLinksUnderTheCursor` and `titleFollowsTheDocument`.

## The offscreen platform requirement

Font metrics come from the platform, so even a completely headless run needs a
`QGuiApplication` — `src/main.cpp` creates a `QApplication` for exactly this
reason, and `LayoutEngine::fontFor()` measures text with `QFontMetricsF`. In a CI
environment or over SSH there is no display, so every test target sets

```cmake
set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
```

`tests/unit/CMakeLists.txt` says so in a comment: "Font metrics need a platform
plugin; offscreen keeps the tests windowless."
`tests/integration/CMakeLists.txt` and `tests/browser/CMakeLists.txt` do the same,
the latter because Qt Widgets need a platform plugin even when no window is shown.

Running a test binary directly therefore needs the variable set by hand:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/tst_layout
```

Consequences a test author must accept: metrics are those of the machine's fonts,
so assertions should use ranges (`QVERIFY(width > 0)`) or arithmetic derived from
the same metrics rather than hard-coded pixel counts. `tst_layout` and
`tst_pipeline` follow that rule.

## How to run

```bash
# From the repository root.
cmake --preset default          # Release, tests ON, build/ directory
cmake --build build -j

ctest --test-dir build --output-on-failure          # all suites
ctest --test-dir build -R tst_http                  # one suite
QT_QPA_PLATFORM=offscreen ./build/bin/tst_css       # one binary, with its own output
```

`CMakePresets.json` defines `default` (Release) and `debug` presets, each with a
matching `build` and `test` preset, so `cmake --workflow --preset default` also
works. `OPENQBROWSER_BUILD_TESTS=OFF` skips the whole `tests/` subtree for a
build that only needs the browser binary; `OPENQBROWSER_WARNINGS_AS_ERRORS=ON`
turns the `-Wall -Wextra -Wpedantic` warnings into errors.

The deterministic report modes are useful companions to the tests during
debugging, because they print the same state the assertions inspect:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-all about:home
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-boxes /dev/stdout page.html
```

## Bug-fixing practice

The suites exist because they were used to find and pin real behaviour. The
project's practice is:

1. Write the failing case as a test first, in the suite that owns the component:
   URL or dot-segment behaviour in `tst_url`, entity or auto-closing behaviour in
   `tst_html`, cascade or selector behaviour in `tst_css`, geometry in
   `tst_layout`, protocol or loading behaviour in `tst_http`, inter-stage
   behaviour in `tst_pipeline`, user-visible behaviour in `tst_browser`.
2. Fix the component, not the test. If the test's expectation is wrong rather than
   the code, change the expectation and say why in the test's comment.
3. Keep the case afterwards. Several tests carry a comment that records the bug
   they were written for — for example the trailing-slash rule in
   `removeDotSegments()` is asserted in `tst_url`, the "first duplicate attribute
   wins" rule in `tst_html`, and the "all three fetches still complete at a
   concurrency limit of one" rule in `tst_http`.
4. Prefer arithmetic assertions over golden images. No test compares a whole
   screenshot; `tst_layout` and `tst_pipeline` count pixels matching a predicate,
   which survives a font or antialiasing difference across machines.

CI should run `ctest` for all three suites; there is no separate lint step, since
the compiler warnings are the lint (`contributing.md` describes the warning setup).
