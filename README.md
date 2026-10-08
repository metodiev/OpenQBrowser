# OpenQBrowser

> An open-source web browser focused on simplicity and privacy.

**OpenQBrowser** renders pages with **Qt WebEngine (Chromium)**, so it shows
modern sites the way Chrome does and plays HTML5 video and audio. The window,
tabs, address bar, bookmarks, history and built-in `about:` pages are
OpenQBrowser's own code on top of it.

The project also contains its original from-scratch pipeline — an HTML parser, a
CSS cascade, a layout engine, a painter and an embedded QuickJS JavaScript
engine — under [`src/`](./src). That code no longer draws the window; it lives
on as a library that the unit and integration test suites exercise, and it is
the reference for anyone who wants to read a browser pipeline end to end.

```
URL → Chromium (network, HTML, CSS, layout, paint, JS, media) → screen
```

## Status

OpenQBrowser is **experimental software**. It is not safe for everyday browsing
and it has no sandbox.

What works today:

| Area | State |
| --- | --- |
| Rendering | Chromium via Qt WebEngine: full HTML/CSS/JavaScript and HTML5 media |
| Browser | Tabs with favicons, back/forward history, bookmarks, built-in `about:` pages, an address bar with search |
| Interface | A themed window that follows the desktop's light or dark appearance, with a tab strip, a flat toolbar and a loading line |
| Tabs | Open with the **+** button, `Ctrl/⌘ + T`, a double click on empty tab-strip space, or a link that asks for a new window; close with the tab's **×** or a middle click |
| DevTools | The Chromium DevTools page, docked in the window (F12) |
| Headless | `--dump-dom` and `--screenshot` drive the same engine the window uses |
| Core library | HTTP, HTML, CSS, layout, QuickJS and storage primitives, covered by the unit and integration suites |

## Status

OpenQBrowser is **experimental software**. It is not safe for everyday browsing
and it has no sandbox.

The original from-scratch pipeline remains in the repository and is still
built and tested: its HTTP client, HTML tokenizer and tree construction, DOM,
CSS tokenizer/selectors/cascade, block/inline/flex/grid layout, painter,
QuickJS bindings and cookie/cache stores are all exercised by the unit and
integration suites under [`tests/`](./tests). It is no longer the rendering
path, but it is the readable reference implementation the project was founded
on.

## Building

### Requirements

* A C++20 compiler
* CMake 3.21 or newer
* Qt 6.5 or newer (Core, Gui, Widgets, Network, Svg, WebEngineWidgets, Test)
* zlib
* A network connection on the **first** build, to fetch QuickJS

QuickJS is downloaded and built by CMake, so it needs no install step. Pass
`-DOPENQBROWSER_SCRIPTING=OFF` to build the core library without it. On macOS,
`brew install qt` provides Qt WebEngine with proprietary media codecs, so H.264
and AAC video play out of the box.

On macOS with Homebrew:

```bash
brew install qt cmake
```

### Build and test

The two scripts in [scripts/](./scripts) wrap the build and the run:

```bash
./scripts/build.sh              # configure and build
./scripts/build.sh --test       # build, then run the test suite
./scripts/build.sh --clean      # delete the build directory first
./scripts/build.sh --debug      # Debug build into build-debug/
```

`run.sh` starts the browser and rebuilds first if the sources are newer than the
binary, so it is the usual way to launch while working on the code:

```bash
./scripts/run.sh                              # open the browser window
./scripts/run.sh https://example.com/         # open a page
./scripts/run.sh --dump-dom https://bbc.com   # any browser flag works
```

Both scripts find Qt on their own (Homebrew on macOS, `CMAKE_PREFIX_PATH`
elsewhere) and handle the macOS toolchain problem described below. Neither is
required — the raw commands work too:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build -j

ctest --test-dir build --output-on-failure
```

### Run

On macOS the build produces an application bundle, so the browser can be started
from Finder like any other app:

```bash
# Launch the app (macOS)
open build/bin/openqbrowser.app

# Or run the bundle's binary directly, which is the same program
./build/bin/openqbrowser.app/Contents/MacOS/openqbrowser
```

On other platforms, or to pass arguments, run the executable:

```bash
# Open the browser window
./build/bin/openqbrowser

# Load a page straight away
./build/bin/openqbrowser https://example.com/
```

On macOS, `OQB_GUI=1 ./scripts/run.sh` starts the bundle through `open` instead,
which gives the window a Dock icon; `OQB_BUILD_DIR=build-debug ./scripts/run.sh`
launches a build from somewhere other than `build/`.

> **Nothing appears?** If you run the plain binary and see no window, make sure
> `QT_QPA_PLATFORM` is not set to `offscreen` in your shell — that environment
> variable is only for headless runs and the tests.

### Making the app easier to launch (macOS)

The bundle lives inside the build directory, which is convenient while working on
the code. It only works there because the Qt frameworks it loads are found in the
Homebrew Qt installation. To produce a self-contained bundle that runs on a
machine without Qt installed, deploy it first:

```bash
./scripts/deploy.sh              # bundle Qt, QtWebEngineProcess and its resources
./scripts/deploy.sh --dmg        # also build a distributable disk image

cp -R build/bin/openqbrowser.app /Applications/
open -a OpenQBrowser
```

`deploy.sh` runs `macdeployqt`, then fixes two things it leaves behind: the
QtWebEngineProcess helper's references to the Homebrew frameworks, and the
frameworks' own `@executable_path` references (which are only correct for the
main executable). The result passes `codesign --verify --deep --strict` and
launches the Chromium renderer from inside the bundle.

To open links in it from other applications, drag the bundle onto the Dock, or add
it under System Settings → Desktop & Dock → Default web browser.

### Headless use

The same engine runs without a window, which is what makes the browser
testable and scriptable.

```bash
# Dump the rendered document (after JavaScript has run)
./build/bin/openqbrowser --dump-dom https://example.com/

# Render to a PNG
./build/bin/openqbrowser --screenshot=page.png --width=1200 https://example.com/
```

On a machine with no display, select the offscreen platform plugin. `run.sh`
does this on its own for `--dump-dom`, so the prefix is only needed when
calling the binary directly:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-dom https://example.com/
```

A screenshot renders through the compositor, so it needs a real platform and
cannot be taken over SSH.

Two toolchains are installed — Xcode's and the Command Line Tools — and their
linkers are different generations. If the link fails with a wall of
`tapi error: malformed file` / `unknown architecture` messages, CMake has paired
a newer SDK with an older linker. Which one you use is chosen by `DEVELOPER_DIR`:

```bash
export DEVELOPER_DIR=/Library/Developer/CommandLineTools
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
```

`scripts/common.sh` does this for you, but only after checking: it links a
trivial program first and only switches if the current environment cannot. See
[architecture/build.md](./architecture/build.md) for the mechanism and an
alternative fix.

## Using the browser

Type in the address bar. OpenQBrowser decides what you meant the way a browser
does: something with a dot in it is a host name, anything else is a search.

| Key | Action |
| --- | --- |
| `Ctrl`/`Cmd`+`L` | Focus the address bar |
| `Alt`+`Left`, `Alt`+`Right` | Back and forward |
| `F5` | Reload |
| `Space`, `Page Up/Down` | Scroll a screenful |
| `Home`, `End` | Jump to the top or bottom of the page |
| `Ctrl`/`Cmd`+click | Open a link in a new tab |
| `F12` | Show or hide the developer tools |

### Developer tools

The **DevTools** button in the toolbar, or **F12**, opens a dock with four tabs:

| Tab | What it shows |
| --- | --- |
| Console | The page's `console` output, and an input line that evaluates JavaScript in the page |
| Elements | The document tree, with the selected element's computed style and the rules that produced it |
| Layout | The document size, the line-box count and the full box tree with geometry |
| Network | Every subresource the page requested, and the failures with their reasons |

**Pick element** arms a crosshair: the next click in the page selects that element
in the tree, which is the quickest way to find out why something looks wrong.

The **Boxes** button draws each box's content, padding and margin over the page.
See [architecture/devtools.md](./architecture/devtools.md).

### Built-in pages

| Page | Contents |
| --- | --- |
| `about:home` | The new tab page |
| `about:about` | A list of every built-in page |
| `about:version` | Version and component details |
| `about:history` | Pages visited in this session |
| `about:bookmarks` | Pages you have saved |
| `about:blank` | An empty document |

These are ordinary HTML documents generated inside the browser, so they render
through exactly the same pipeline as a network page. If the new tab page looks
wrong, the bug is in the renderer.

## Repository layout

```
OpenQBrowser/
├── src/
│   ├── ui/          Window, tab strip, theme and the WebEngine-backed tabs
│   │   ├── Theme      Colours, stylesheet and the painted icons
│   │   ├── TabBar     The tab strip: pills, close buttons, new-tab gestures
│   │   ├── WebTab     One tab: a Chromium view plus address and history glue
│   │   └── MainWindow The chrome: tab strip, toolbar, address bar, DevTools
│   ├── browser/     Built-in about: pages (rendered by Chromium)
│   ├── network/     URLs, HTTP/1.1, HTTPS, redirects, resource loading
│   ├── dom/         Document Object Model
│   ├── html/        HTML tokenizer, tree construction, entities
│   ├── css/         CSS tokenizer, selectors, cascade, media queries
│   ├── renderer/    Box tree, layout, painting
│   ├── javascript/  QuickJS engine, DOM bindings, events, timers
│   ├── storage/     History and bookmarks
│   ├── security/    Origin, transport and permission policy
│   ├── devtools/    The from-scratch inspector (core library)
│   └── main.cpp     Command line interface
├── tests/
│   ├── unit/        URL, HTML, CSS, layout and JavaScript
│   ├── integration/ HTTP, the full pipeline and scripted pages
│   └── ui/          The window: tabs, icons and the chrome
├── examples/        Pages that exercise the from-scratch engine
├── architecture/    Design documents for every subsystem
├── scripts/         build.sh, run.sh and deploy.sh
├── assets/          Icons and other static files
├── CMakeLists.txt
└── LICENSE
```

The window is `src/ui`: a themed tab strip, a toolbar with the address bar, and
one `WebTab` per tab. Each tab is a Qt WebEngine view, so pages render and play
media the way Chrome does. `src/browser` contributes the built-in `about:` pages,
which are ordinary HTML documents styled to match the chrome.

`src/network` through `src/browser` also form a static library, `oqb_core`, that
the window no longer draws with: it is the from-scratch pipeline, kept compiled
and covered by the unit and integration suites.

## Testing

Nineteen suites, run by `ctest`. Each suite is also a standalone executable in
`build/bin`, so a single file can be run on its own.

| Suite | Covers |
| --- | --- |
| `tst_url` | URL parsing and RFC 3986 reference resolution |
| `tst_html` | Tokenizer, tree construction, entities, quirks mode |
| `tst_css` | Tokenizer, values, selectors, cascade, media queries, `@media` evaluation |
| `tst_grid` | Grid track parsing and sizing |
| `tst_layout` | Box tree, block, inline, flex and grid layout, painting |
| `tst_javascript` | The engine, bindings, events, timers |
| `tst_cookies` | Cookie parsing, the cookie jar, `document.cookie` |
| `tst_cache` | Cache directives and validation |
| `tst_fetch` | `fetch()` policy: methods, headers, CORS, credentials |
| `tst_http` | The HTTP client against a local server: framing, redirects, errors |
| `tst_pipeline` | Parse → style → layout → paint, end to end |
| `tst_redirect` | Redirect chains, late stylesheets, large documents, nesting limits |
| `tst_perf` | Layout cost, to catch worse-than-linear behaviour |
| `tst_cookie_flow` | A `Set-Cookie` travelling through a real response |
| `tst_cache_flow` | Revalidation and conditional requests over HTTP |
| `tst_fetch_flow` | A script calling `fetch()` through the page loader |
| `tst_scripting` | Scripts running in the pipeline, in the right order |
| `tst_tabs` | The window's tabs: opening, closing, switching, icons, shortcuts |
| `tst_theme` | The palette, the stylesheet and every painted icon |

`tst_tabs` also writes a screenshot of the window when `OQB_UI_PREVIEW` names a
file, which is how the chrome is reviewed by eye:

```bash
OQB_UI_PREVIEW=/tmp/browser.png ./build/bin/tst_tabs writePreview
```
| `tst_devtools` | The inspector panel and its reports |

The tests are not decoration: they have found and fixed real bugs, including
several where the parser or layout engine would loop or misplace content. Layout
and media-query behaviour is checked against Chrome on identical markup, so the
suites encode what other browsers actually do rather than what the code happens
to do. See [architecture/testing.md](./architecture/testing.md).

## Documentation

The [architecture/](./architecture) directory holds the design documents. Each
one describes a subsystem, its data structures and its known limits.

Start with [architecture/overview.md](./architecture/overview.md), or jump to
the subsystem you care about:

* [overview.md](./architecture/overview.md) — the whole system and the pipeline
* [networking.md](./architecture/networking.md) — HTTP, TLS, redirects, caching
* [html.md](./architecture/html.md) — tokenizing and tree construction
* [css.md](./architecture/css.md) — selectors, the cascade, media queries
* [rendering.md](./architecture/rendering.md) — box tree, layout, painting
* [browser.md](./architecture/browser.md) — pages, tabs, history
* [javascript.md](./architecture/javascript.md) — the engine, the DOM bindings and script ordering
* [security.md](./architecture/security.md) — what is enforced, and what is missing
* [storage.md](./architecture/storage.md) — history and bookmarks today
* [devtools.md](./architecture/devtools.md) — the console, the element inspector and the picker
* [testing.md](./architecture/testing.md) — the suites and what they catch
* [build.md](./architecture/build.md) — the build system and its specifics
* [contributing.md](./architecture/contributing.md) — the workflow and conventions

## Contributing

Contributions are welcome. The short version:

1. Fork the repository and create a feature branch.
2. Make the change, following the conventions in [architecture/contributing.md](./architecture/contributing.md).
3. Add or update tests. A change without a test is a change that will break.
4. Add any new source file to [`src/CMakeLists.txt`](./src/CMakeLists.txt), or it
   will not be compiled.
5. Run the test suite.
6. Open a pull request.

```bash
git checkout -b feature/my-change
./scripts/build.sh --test
git commit -am "Describe the change"
git push origin feature/my-change
```

Please keep changes focused and document architectural decisions in the
relevant document under `architecture/`.

## Security

Security is a core part of OpenQBrowser, and its current state is deliberately
conservative:

* TLS certificates are verified and there is no way to bypass a failure.
* A secure page cannot load an insecure subresource.
* Permissions such as camera and geolocation are denied, with no way to grant
  them yet.

Do **not** assume experimental browser code is safe for everyday browsing. Until
the security architecture is mature — sandboxing, content security policy and
process isolation are all still missing — treat OpenQBrowser as a research
project.

If you find a vulnerability, please report it privately rather than disclosing it
publicly before a fix is available.

## License

OpenQBrowser is released under the MIT License. See [`LICENSE`](./LICENSE).
