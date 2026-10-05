# OpenQBrowser architecture

These documents describe how OpenQBrowser is actually built: which files do what,
what the code guarantees today, and what it deliberately does not do. They are
written against the source tree, not against a roadmap. Where something is
missing the document says so and describes what a reader will observe.

The browser is a C++20/Qt 6 program that implements its own URL parsing, HTTP/1.1
client, HTML tokenizer and tree builder, CSS cascade, box tree, layout and
painting. It does not embed a third-party rendering engine, and it is not a
wrapper around `QWebEngineView`.

## Documents

| Document | What it covers |
| --- | --- |
| `overview.md` | Goals, the pipeline from URL to pixels, the module map, the dependency rule and the data flow. |
| `networking.md` | `Url`, `HeaderList`, `HttpRequest`/`HttpResponse`, `HttpClient`'s state machine, body framing, content encodings, redirects, timeouts and TLS. |
| `html.md` | The tokenizer's text modes, entity decoding, tree construction, implied elements and auto-closing, quirks mode and the implementation's limits. |
| `css.md` | The CSS tokenizer, the `Value` model, selectors and specificity, stylesheet parsing, media queries, the cascade, inheritance and presentational hints. |
| `rendering.md` | The box tree, anonymous boxes, block and inline layout, margin collapsing, shrink-to-fit, replaced elements, tables and painting. |
| `browser.md` | `Page` and `Tab`, the two-phase load and its signals, built-in `about:` pages, history semantics, bookmarks and the error page taxonomy. |
| `javascript.md` | An honest status report: JavaScript is not implemented, what happens instead, and what the `ScriptEngine` seam is for. |
| `security.md` | TLS verification with no bypass, no insecure subresources from a secure page, same-origin, cookies and the default-deny permission policy. |
| `storage.md` | The in-memory history and bookmark stores, and the design constraints for adding cookies, a cache and persistence. |
| `testing.md` | The three test suites and their nine binaries, the loopback HTTP test server and the offscreen platform requirement. |
| `build.md` | CMake targets, the `oqb_core`/`oqb_ui` split, the presets, tests and the macOS-specific setup that actually matters. |
| `contributing.md` | Workflow, module layout, the `src/CMakeLists.txt` rule, the coding style in the tree, and the expectation that changes come with tests. |

## Reading order

1. `overview.md` — the shape of the whole thing.
2. `networking.md`, `html.md`, `css.md`, `rendering.md` — the pipeline, in the
   order data flows through it.
3. `browser.md` — how those stages are driven for one navigation and one tab.
4. `security.md`, `storage.md` — cross-cutting concerns that touch several stages.
5. `javascript.md` — what is missing, before reading the rest of the limits.
6. `testing.md`, `build.md`, `contributing.md` — how to work on the code.

## How to check the claims

Every document names real identifiers and file paths. A useful sanity check while
reading is to run the browser headlessly:

```bash
cmake --preset default
cmake --build build -j
QT_QPA_PLATFORM=offscreen ./build/bin/openqbrowser --dump-all about:home
```

`--dump-dom`, `--dump-layout`, `--dump-boxes`, `--dump-styles` and `--screenshot=FILE`
all come from `src/main.cpp` and print the state of the pipeline described here.
