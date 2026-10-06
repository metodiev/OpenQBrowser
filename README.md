# OpenQBrowser

> An open-source web browser built from scratch, focused on simplicity, privacy,
> performance, and learning how browsers work under the hood.

**OpenQBrowser** is an experimental browser project built from the ground up. It
fetches pages, parses HTML, applies CSS, lays out the result and paints it, using
its own implementation of every stage rather than an existing engine. The goal is
to make the browser pipeline readable end to end: you can follow a URL from the
address bar to the pixels on screen by reading the source in order.

It is built for developers, researchers, students, and anyone who wants to see
how a browser works rather than only use one.

```
URL → DNS → TCP/TLS → HTTP → HTML parse → DOM → CSS cascade → box tree
    → layout → paint → screen
```

Every arrow in that chain is a directory under [`src/`](./src), and each stage
depends only on the stages before it.

## Status

OpenQBrowser is **experimental software**. It is not safe for everyday browsing
and it has no sandbox.

What works today:

| Area | State |
| --- | --- |
| Networking | HTTP/1.1 and HTTPS, redirects, chunked bodies, gzip/deflate |
| HTML | Tokenizer and tree construction, including implied elements |
| DOM | Element, text and comment nodes with attributes and classes |
| CSS | Tokenizer, selectors with specificity, the cascade, media queries |
| Rendering | Block, inline, flex, float and grid layout, margin collapsing, box-sizing, painting, images |
| Positioning | `relative`, `absolute`, `fixed` and `sticky`, with offsets and `z-index` |
| JavaScript | QuickJS embedded: the DOM bindings, events and timers; inline, external, `defer` and `async` scripts |
| Browser | Tabs, back/forward history, bookmarks, built-in `about:` pages |
| Cookies | A cookie jar: `Set-Cookie` parsing, host and path scoping, expiry, `Secure`, `HttpOnly`, `SameSite` |
| Caching | Conditional caching: `Cache-Control`, `Expires` and `Last-Modified` freshness, and `ETag` revalidation, so a stale resource costs a round trip and no body |
| DevTools | Console that evaluates in the page, element tree with computed style and applied rules, element picker, layout, resource and cookie views |
| `fetch` | Promise-based `fetch` with `Response` and `Headers`: `text()`, `json()`, `arrayBuffer()`, `blob()`, GET/POST/HEAD, request headers and bodies, CORS, and the browser's own header safeguards |
| Testing | Nineteen test suites, all passing |

What does not work yet, and what you will observe:

| Area | What happens |
| --- | --- |
| Modules | `<script type="module">` is skipped, so a page written as modules needs a bundler or a fallback. |
| `XMLHttpRequest`, CORS preflight | `fetch` is implemented and `XMLHttpRequest` is absent, so a page takes its modern path. A request that would need an `OPTIONS` preflight is refused rather than negotiated. |
| Forms | Rendered and styled, but nothing is submitted. |
| Persistence | Cookies and cached responses work but are held in memory; nothing — cookies, cache, history or bookmarks — is stored between runs. |
| Sandboxing | No process isolation or site isolation. |

## Building

### Requirements

* A C++20 compiler
* CMake 3.21 or newer
* Qt 6.5 or newer (Core, Gui, Widgets, Network, Test)
* zlib
* A network connection on the **first** build, to fetch QuickJS

QuickJS is downloaded and built by CMake, so it needs no install step. Pass
`-DOPENQBROWSER_SCRIPTING=OFF` to build without it; the browser still renders and
reports that scripts do not run.

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
the code. To keep a copy in Applications:

```bash
cp -R build/bin/openqbrowser.app /Applications/
open -a OpenQBrowser
```

To open links in it from other applications, drag the bundle onto the Dock, or add
it under System Settings → Desktop & Dock → Default web browser.

### Headless use

The same pipeline runs without a window, which is what makes the browser
testable and scriptable. These modes are how the tests and the examples in this
repository are checked.

```bash
# Dump the parsed document tree
./build/bin/openqbrowser --dump-dom https://example.com/

# Dump the box tree with geometry
./build/bin/openqbrowser --dump-layout https://example.com/

# Dump the computed style of every element
./build/bin/openqbrowser --dump-styles https://example.com/

# Everything at once, which is the same report the DevTools panel shows
./build/bin/openqbrowser --dump-all https://example.com/

# Render to a PNG
./build/bin/openqbrowser --screenshot=page.png --width=1200 https://example.com/
```

On a machine with no display, select the offscreen platform plugin. `run.sh`
does this on its own for the report flags, so the prefix is only needed when
calling the binary directly:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --screenshot=page.png about:home
```

### On macOS

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
│   ├── network/     URLs, HTTP/1.1, HTTPS, redirects, resource loading
│   ├── dom/         Document Object Model
│   ├── html/        HTML tokenizer, tree construction, entities
│   ├── css/         CSS tokenizer, selectors, cascade, media queries
│   ├── renderer/    Box tree, layout, painting
│   ├── browser/     Page and Tab orchestration, built-in pages
│   ├── javascript/  QuickJS engine, DOM bindings, events, timers
│   ├── storage/     History and bookmarks
│   ├── security/    Origin, transport and permission policy
│   ├── devtools/    The inspector
│   ├── ui/          Qt Widgets shell: window, tabs, address bar
│   └── main.cpp     Command line interface
├── tests/
│   ├── unit/        URL, HTML, CSS, layout and JavaScript
│   ├── integration/ HTTP, the full pipeline and scripted pages
│   └── browser/     The window, tabs and navigation
├── examples/        Pages that exercise the engine
├── architecture/    Design documents for every subsystem
├── scripts/         build.sh and run.sh
├── assets/          Icons and other static files
├── CMakeLists.txt
└── LICENSE
```

`src/network` through `src/browser` form a static library, `oqb_core`, with no
dependency on any windowing toolkit. That is what lets a page be fetched, parsed,
styled and laid out headlessly, and it is why the command line dumps and the
tests produce the same result as the window. `src/ui` is a second library on top
of it.

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
| `tst_browser` | The window, tabs, history, error pages |
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
