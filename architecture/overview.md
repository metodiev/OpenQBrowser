# System overview

OpenQBrowser is a web browser built from scratch in C++20 on top of Qt 6. It
implements the browser pipeline itself — URL parsing, HTTP/1.1, HTML parsing, CSS,
layout and painting — so that every stage is small enough to read and explain.
Qt supplies a `QGuiApplication`, `QWidget` shell, platform font metrics, sockets
and TLS, image decoding, and a `QPainter` to draw into. Qt does not supply any part
of the web platform.

## Goals

* **Legible stages.** Each stage is a directory under `src/` with a narrow public
  header, and each stage depends only on the stages before it.
* **Explicit behaviour.** OpenQBrowser parses URLs, HTML and CSS itself rather
  than delegating to `QUrl`, `QTextDocument` or `QNetworkAccessManager`, because
  the point of the project is that the behaviour is visible in the source.
* **Honest failure.** Anything unsupported is reported — a console message, an
  error page, an inspector warning — rather than silently approximated.
* **Headless first.** The whole pipeline runs without a window, which is what
  makes the tests and the `--dump-*` command line modes possible.

Non-goals today: process isolation, sandboxing, network caching, and exact
conformance with the HTML, CSS and URL standards.

## The pipeline from URL to pixels

```
  user input / link
        |
        v
  Url::fromUserInput / Url::parse        network/Url.cpp        RFC 3986
        |
        v
  SecurityPolicy::checkTopLevelNavigation   security/SecurityPolicy.cpp
        |
        v
  ResourceLoader::fetch                  network/ResourceLoader.cpp   cache + queue
        |
        v
  HttpClient::send                       network/HttpClient.cpp       HTTP/1.1 over TCP or TLS
        |
        v
  HttpResponse (Content-Length / chunked / read-until-close, gzip, deflate)
        |
        v
  html::Parser::parse                    html/Parser.cpp        tokenizer -> DOM
        |
        v
  dom::Document                          dom/Document.cpp       node tree, title, base URL
        |
        v
  css::StyleEngine::computeStyles        css/Cascade.cpp        selectors, cascade, inheritance
        |
        v
  renderer::BoxTreeBuilder::build        renderer/BoxTree.cpp   boxes + anonymous boxes
        |
        v
  renderer::LayoutEngine::layout         renderer/Layout.cpp    absolute geometry
        |
        v
  renderer::Painter::paint / PageView::paintEvent   renderer/Painter.cpp, ui/PageView.cpp
        |
        v
  pixels
```

Subresources (external stylesheets and images) are fetched after the document is
parsed and re-run through the CSS and layout stages; see `browser.md`.

## Module map

| Module | Public header | Responsibility |
| --- | --- | --- |
| `src/network` | `network/Url.h`, `HttpMessage.h`, `HttpClient.h`, `ResourceLoader.h` | RFC 3986 URLs, HTTP/1.1 and HTTPS, redirects, encodings, resource cache. |
| `src/dom` | `dom/Node.h`, `dom/Document.h` | Node tree with attributes and classes; document metadata. |
| `src/html` | `html/Tokenizer.h`, `html/Parser.h`, `html/Entities.h` | Tokenizer, character references, tree construction. |
| `src/css` | `css/Tokenizer.h`, `css/Selector.h`, `css/Value.h`, `css/Stylesheet.h`, `css/Style.h` | Tokenizer, value model, selectors, stylesheet parsing, media queries, cascade. |
| `src/renderer` | `renderer/BoxTree.h`, `Layout.h`, `Painter.h` | Box tree, layout, painting. |
| `src/storage` | `storage/History.h`, `storage/Bookmarks.h` | History list with a cursor; bookmark list. |
| `src/security` | `security/SecurityPolicy.h` | Origin checks, subresource downgrade rules, permissions, transport security. |
| `src/javascript` | `javascript/ScriptEngine.h`, `Engine.h`, `Bindings.h` | The embedded QuickJS engine, its DOM bindings, events and timers. |
| `src/devtools` | `devtools/Inspector.h` | Text views of the DOM, styles, box tree and layout. |
| `src/browser` | `browser/Page.h`, `Tab.h`, `BuiltinPages.h`, `PageSettings.h` | One navigation (`Page`), one tab (`Tab`), `about:` documents, user settings. |
| `src/ui` | `ui/MainWindow.h`, `ui/PageView.h` | Qt Widgets shell: window, tabs, toolbar, and the widget that paints a page. |

## The dependency rule

Each stage depends only on earlier stages, and the arrow never points backwards:

```
  network  ->  dom  ->  html  ->  css  ->  renderer  ->  browser  ->  ui
                ^       |         |          |
                +-------+---------+----------+   (earlier stages never include later ones)
```

Concretely:

* `network/Url.h` includes only Qt strings.
* `dom/Node.h` includes Qt containers; `dom/Document.h` includes `network/Url.h`
  so a document knows its own address.
* `html/Parser.h` includes `dom/Document.h` and `network/Url.h`.
* `css/Style.h` includes `css/Stylesheet.h` and forward-declares `dom::Element` and
  `dom::Document`.
* `renderer/BoxTree.h` includes `css/Style.h` and `dom/Node.h`.
* `browser/Page.h` includes all of the above plus `javascript/ScriptEngine.h`,
  `storage/History.h` and `storage/Bookmarks.h`.
* `ui/PageView.h` includes `network/Url.h` and forward-declares `browser::Tab`.

The rule has two payoffs. First, `oqb_core` has no windowing dependency, so a page
can be fetched, parsed, styled and laid out with no display (see `build.md`).
Second, a cycle in the graph is a design error that is visible at compile time
rather than a runtime surprise.

## Data flow for one navigation

```
Page::load(url)
  |
  +-- about: URL?        -> BuiltinPages::documentFor() -> Resource -> buildDocument()
  |
  +-- otherwise          -> ResourceLoader::fetch(url)
                              |
                              +-- cache hit -> emit finished(resource) immediately
                              +-- file://   -> loadLocalFile()
                              +-- http(s)   -> HttpClient -> HttpResponse
         |
         v
Page::handleDocumentResource()
  |  ok / HTTP error with a body -> buildDocument()
  |  transport failure           -> finishWithError(kind, details) -> builtin::errorPage()
  v
Page::buildDocument()
  |  1. security::SecurityPolicy::checkTopLevelNavigation()
  |  2. html::Parser::parse()               -> dom::Document
  |  3. ScriptEngine::execute() per <script>  (recorded, not run)
  v
Page::collectSubresources()   <link rel=stylesheet>, <img src>, max 200
Page::buildLayout()           StyleEngine -> BoxTreeBuilder -> LayoutEngine
  |
  emit Page::ready()          (paintable now, subresources may still be arriving)
  |
  v
Page::handleSubresource()     late CSS re-runs the cascade; late images re-lay out
  |
  emit Page::finished()       -> storage::HistoryStore::visit()
```

`Tab` wraps a `Page`, connects these signals to its own `stateChanged`,
`titleChanged`, `loadStarted`, `loadFinished` and `loadFailed` signals, and moves
the shared `HistoryStore` cursor for back and forward. `ui::MainWindow` owns one
`Tab` and one `ui::PageView` per tab and reflects the tab state in the toolbar.

## Key entry points

| Task | Function |
| --- | --- |
| Turn typed text into a URL or a search | `oqb::network::Url::fromUserInput()` |
| Fetch one resource | `oqb::network::ResourceLoader::fetch()` |
| Parse HTML | `oqb::html::Parser::parse()` |
| Compute style | `oqb::css::StyleEngine::computeStyles()` |
| Build boxes | `oqb::renderer::BoxTreeBuilder::build()` |
| Lay out | `oqb::renderer::LayoutEngine::layout()` |
| Paint | `oqb::renderer::Painter::paint()` and `ui::PageView::paintEvent()` |
| Load a page | `oqb::browser::Page::load()` |
| Inspect a page from a terminal | `oqb::devtools::Inspector::fullReport()` |

## Where the design is thin

The stages are real but partial, and each has its own document:

* No module scripts, no `fetch`, and no Web Storage; see [javascript.md](javascript.md).
* `display: grid` is parsed but laid out as ordinary blocks; flexbox is implemented.
* Table display types are parsed and boxed, but there is no table layout algorithm.
* `float` is stored on the style and never used for placement.
* Positioning is implemented, but there is no nested scroll container for `sticky` to use.
* No sandbox, no content security policy, no process isolation.

`javascript.md`, `security.md` and `rendering.md` describe each of these in full.
