# Browser orchestration

`src/browser/` decides what a navigation *is*. It contains the page lifecycle
(`Page`), the per-tab navigation state (`Tab`), the pages served from inside the
browser (`BuiltinPages`), and the user's settings (`PageSettings`). The window on
top of it lives in `src/ui/`.

| File | Contents |
| --- | --- |
| `src/browser/Page.h`, `Page.cpp` | `Page`: one navigation, the two-phase load, the error path. |
| `src/browser/Tab.h`, `Tab.cpp` | `Tab`: navigation state, back/forward, address-bar input. |
| `src/browser/PageSettings.h` | `PageSettings`: viewport, fonts, colours, search template, limits, toggles. |
| `src/browser/BuiltinPages.h`, `BuiltinPages.cpp` | `about:` documents and `errorPage()`. |
| `src/ui/MainWindow.h`, `MainWindow.cpp` | Tabs, toolbar, address bar, bookmarks button, box-model toggle. |
| `src/ui/PageView.h`, `PageView.cpp` | The widget that paints a page, scrolls it and turns clicks into navigation. |

`Page` owns the whole pipeline for one navigation. Every object it needs is a
member (`m_loader`, `m_document`, `m_styles`, `m_boxTree`, `m_layout`,
`m_scripts`), so a page can be created, driven to completion and queried with no
global state. That is what makes the headless mode, the tests and the windowed
mode share one code path.

## The load lifecycle

```
Page::load(url)
   state = LoadingDocument
   loader->fetch(url)
        |
        |  ResourceLoader::finished(resource)
        v
Page::handleDocumentResource(resource)
   |-- transport failure (no body)         -> finishWithError(kind, message)
   |-- HTTP >= 400 with a body             -> buildDocument(body), record failedResource
   |-- ok                                  -> buildDocument(body)
        |
        v
Page::buildDocument(resource)
   security::SecurityPolicy::checkTopLevelNavigation()      -> refuse -> finishWithError("blocked")
   Resource::text() -> html::Parser::parse()                 -> dom::Document
   ScriptEngine::execute() for each <script>                 -> recorded, never run
   state = LoadingSubresources
   collectSubresources()                                     -> up to 200 URLs
   buildLayout()                                             -> styles, boxes, geometry
   emit ready()
   |-- nothing to fetch -> recordHistory(); state = Idle; emit finished()
   `-- else             -> requestNextSubresource()
        |
        v
Page::handleSubresource(resource)   (one per resource)
   CSS  -> stylesheet added, buildLayout(), emit ready() again
   image-> intrinsic size attached to the box, buildLayout(), emit ready() again
   last one -> recordHistory(); state = Idle; emit finished()
```

Signals:

| Signal | When |
| --- | --- |
| `started(url)` | Immediately in `load()`, before any bytes. |
| `redirected(from, to)` | Declared for redirect reporting. It is currently never emitted by `Page`; the network layer's own `HttpClient::redirected` is the signal that fires. |
| `ready()` | After the document has been parsed, styled and laid out — paintable, even if subresources are still arriving. Fires again for each late stylesheet or image. |
| `finished()` | Once the load is as complete as it will get, after `recordHistory()`. |
| `failed(message)` | When an error page has been built; `ready()` and `finished()` follow, so the error page renders like any other document. |
| `titleChanged(title)` | Declared for the document title. `Page::title()` is what the tab and the window call; this signal is not emitted by `Page` today, so `Tab::connectPage()` synthesises `titleChanged` from `ready` and `finished` instead. |

`Tab::connectPage()` maps these onto `loadStarted`, `stateChanged`, `titleChanged`,
`loadFinished` and `loadFailed`, which is what the window listens to
(`ui/MainWindow.cpp`).

### Why the load is two-phase

The document and its subresources are deliberately separate phases. The document
is parsed and laid out first and `ready()` is emitted before any subresource is
requested, so:

* the page is visible and scrollable as soon as the HTML arrives, rather than
  waiting for every image on a slow connection;
* a stylesheet that arrives late still takes effect — the cascade runs again and
  `ready()` fires a second time, which the window treats as a repaint;
* an image that arrives late is measured and the page re-lays out, so it takes its
  intrinsic size;
* a subresource failure is recorded in `failedResources()` rather than replacing
  the page with an error.

`collectSubresources()` walks `<link rel="stylesheet">` and `<img src>` in
document order (so author stylesheets keep their order), skips `data:` image URLs,
and stops at `kMaxSubresources` (200) so a hostile or generated page cannot queue
unbounded work. `PageSettings::loadExternalStylesheets` and `loadImages` turn each
category off, which is what `--no-images` on the command line sets.

`requestNextSubresource()` hands every URL to the loader at once; the loader's own
queue enforces `PageSettings::maxConcurrentRequests` (default 6). Each completion
decrements `m_inFlightSubresources`, and the load finishes when both the in-flight
count and the pending list are empty.

`Page::reload()` simply re-loads `m_documentUrl`; `Page::stop()` calls
`ResourceLoader::cancelAll()` and `clearCache()`, drops the pending list and
returns the state to `Idle` — which is why a reload after a stop re-fetches
instead of reusing the cache.

### Scripts

`buildDocument()` scans `document->getElementsByTagName("script")` and calls
`m_scripts->execute()` for each one: the `src` resolved against the document URL
for external scripts, the element's `textContent()` for inline ones. The engine
records a console message and adds the script to `skippedScripts()` and returns;
nothing is executed. See `javascript.md`.

### Title, history recording and viewport

* `Page::title()` prefers `Document::title()` (`<title>`, then the first `h1`–`h3`),
  then the error message, then `Url::displayHost()`.
* `recordHistory()` calls `HistoryStore::visit(documentUrl, title())` when
  `PageSettings::recordHistory` is on, a history store is attached and the page is
  not an error page. It runs at the end of the load, so the title is known.
* `buildLayout()` is where `PageSettings` reaches the pipeline: viewport size and
  `defaultFontSize` build the `css::StyleContext`, `defaultBackground` and
  `defaultTextColor` go to `StyleEngine::setBaseColors()`,
  `prefersDarkScheme` reaches media queries. Every `<style>` element is parsed as
  an author sheet in document order, then network stylesheets are re-parsed from
  `ResourceLoader`'s cache — which is why the cascade is reconstructed from
  scratch on each `buildLayout()` and cannot go stale.

`Page::renderToImage()` runs the same `Painter` the window does, with the page's
own viewport, background and text colours; that is what `--screenshot` and the
tests use.

## about: pages are generated in the browser

`builtin::documentFor(url, version, viewportWidth, history, bookmarks, tabCount)`
returns an HTML string for each built-in page, and `Page::loadBuiltinPage()`
wraps it in a `Resource` with `text/html; charset=utf-8` and a status of 200
before calling the same `buildDocument()` a network document would use.

| Name | Contents |
| --- | --- |
| `about:home` (or an empty name) | The new tab page: title, search hint, the list of built-in pages, and how many tabs are open. |
| `about:about` | The list of built-in pages with one-line descriptions and a note that the browser is unaudited and has no sandbox. |
| `about:version` | The version badge and the component list (`network`, `html`, `css`, `dom`, `renderer`, `browser`, `javascript`). |
| `about:history` | `HistoryStore` read live, newest first, with visit counts. |
| `about:bookmarks` | `BookmarkStore::toHtml()`, grouped by folder. |
| `about:blank` | An empty document. |

An unknown `about:` name becomes the `notfound` error page rather than a blank
one, so the user learns the page does not exist.

Two reasons these are HTML rather than native widgets, both stated in
`BuiltinPages.h`: they render through exactly the same pipeline as a network page,
so a wrong-looking new tab page is a renderer bug rather than something hidden
behind a widget; and they work offline and cannot be redirected by the network.
`MainWindow` wires them to menu entries (`Pages ▸ New Tab Page / History /
Bookmarks / Version`) and the bookmark star to the toolbar.

## History: back and forward semantics

`storage::HistoryStore` is a list of `HistoryEntry` (`url`, `title`, `visitedAt`,
`visitCount`) plus a cursor, `m_currentIndex`. A `Tab` holds no list of its own: it
calls `HistoryStore::goBack()`/`goForward()` and then loads
`HistoryStore::currentUrl()`. That is what keeps the back button and the history
page in agreement, and it is why the comments in `Tab.h` describe the cursor as the
tab's navigation state.

`visit()` implements the two rules every browser has:

1. **Reload is not a new step.** Re-visiting the URL at the cursor updates that
   entry's `visitedAt` and `visitCount` and its title, instead of appending.
2. **Forward truncation.** Navigating while the cursor is not at the end removes
   every entry after it with `while (m_entries.size() > m_currentIndex + 1)
   m_entries.removeLast()`, then appends and moves the cursor to the new entry.

The list is capped at `HistoryStore::kMaxEntries` (5000); when it overflows the
oldest entries are dropped and the cursor is decremented to match. `clear()`,
`search(text, limit)` (most recent first, matching URL or title),
`mostVisited(limit)` (visit counts, ties broken by recency) and `describe()` (the
plain-text listing `about:history` uses) round out the store. `goTo(index)` moves
the cursor without recording a visit.

`Tab::goBack()`/`goForward()` set `m_navigatingFromHistory = true` before loading,
so the movement itself cannot add a step. `Tab::canGoBack()`/`canGoForward()`
delegate to the store, and `MainWindow::updateNavigationState()` enables the
toolbar buttons from them.

Note that the history is **shared by every tab** in the window, because
`MainWindow` owns one `HistoryStore` and passes it to each `Tab::setHistory()`.
That matches the comment in `Tab.h`, and it means back/forward is a window-wide
cursor rather than a per-tab one.

## Bookmarks

`storage::BookmarkStore` holds `Bookmark` values (`url`, `title`, `addedAt`,
`folder`). `add()` updates an existing entry for the same URL instead of
duplicating it, filling in the title with `Url::displayHost()` when none is given;
`remove()`, `contains()`, `toggle()` (returns the new state), `inFolder()`,
`folders()`, `search()` and `toHtml()` complete the API. The toolbar's star
button in `MainWindow::onToggleBookmark()` is the only writer, and the star glyph
reflects `contains(tab->url())`.

## Error page taxonomy

`Page::finishWithError(kind, details)` builds an HTML document with
`builtin::errorPage(url, kind, details)`, parses and lays it out through the normal
pipeline, sets `m_errorPage`/`m_error`, then emits `ready()`, `failed(details)` and
`finished()`. Because the error page goes through the same path, it is scrollable,
has a title, and can be screenshotted.

`kind` comes from `classifyFailure(message)`, which matches on the transport error
text, or from the code path that failed:

| `kind` | Trigger | Heading |
| --- | --- | --- |
| `dns` | Error mentions host-not-found, name resolution, no-such-host or DNS. | Server not found |
| `timeout` | Error mentions "timed out". | The site took too long to respond |
| `tls` | Error mentions certificate, TLS or SSL. | Secure connection failed |
| `connect` | Error mentions refused, closed or connection. | Cannot reach this site |
| `network` | Anything else, including a document that could not be parsed. | This page could not be loaded |
| `blocked` | `SecurityPolicy::checkTopLevelNavigation()` refused the URL. | This address was blocked |
| `notfound` | An unknown `about:` page. | Page not found |
| `http` | Defined in `errorPage()` for a server error that has no body to show. | The server returned an error |

Each page carries the URL, an explanation naming the host, a hint, and — except
for `notfound` — the raw `details` in a card, plus a link back to `about:home`.

An HTTP error status is special-cased in `handleDocumentResource()`: when the
server sent a body, that body is rendered and the failure is only recorded in
`failedResources()` and the status bar. Browsers show the server's own error page,
not a browser-generated one, and so does OpenQBrowser.

## The window

`ui::MainWindow` owns the `PageSettings`, one shared `HistoryStore` and one shared
`BookmarkStore`, a `QTabWidget` whose pages are `ui::PageView` widgets, a toolbar
(back, forward, reload, stop, address bar, bookmark, box-model toggle), a status
bar with a busy indicator, and menus (File: new tab, close tab, quit; View: the
navigation actions and the box-model overlay; Pages: the built-in pages).

Keyboard and mouse behaviour lives in `ui::PageView`: scroll on wheel, space,
arrows, Page Up/Down, Home/End; `F5` emits `reloadRequested()`; a press followed
by a release within 4 pixels counts as a click and emits `linkActivated(url,
newTab)` where `newTab` is a Ctrl- or Cmd-click; hovering a link emits
`linkHovered()` and sets the pointing-hand cursor. `setTab()` connects the view to
the tab's `stateChanged` and `loadFinished` and resets the scroll position on a new
document.

`MainWindow::syncTabs()` keeps `Tab::setTabCount()` in step so the new tab page's
"n tabs open" line is right, and `updateNavigationState()` re-reads the tab on
every `stateChanged` to enable the buttons, update the address bar (only when it
does not have focus, so typing is not interrupted) and the bookmark star.

The window is opt-in on the command line (`--window`), and running the binary with
no arguments implies it; any `--dump-*` or `--screenshot` option keeps it
headless (see `build.md`).
