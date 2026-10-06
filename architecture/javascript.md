# JavaScript

OpenQBrowser runs page script. A page's `<script>` elements are fetched, ordered
and executed against the real DOM, and the changes they make are re-styled,
re-laid out and painted. The engine is [QuickJS](https://github.com/quickjs-ng/quickjs),
a small embeddable ES2023 implementation, chosen because it is a real engine
with a small C API, an MIT licence, and no dependency the browser would not
already have.

This document describes how the engine is embedded, how the DOM is bound into
it, and what is deliberately not supported.

## Why an embedded engine

Writing a JavaScript engine is a project of its own, and a browser whose script
support is half-working is worse than one that says so. Embedding an existing
engine means the language is correct by construction, and the work that remains
is the part specific to this browser: binding the DOM, and getting the *timing*
right, which is where a browser's observable behaviour actually lives.

The engine is fetched and built by CMake, so a checkout needs no manual setup:

```cmake
option(OPENQBROWSER_SCRIPTING "Run page JavaScript with the embedded QuickJS engine" ON)
set(OPENQBROWSER_QUICKJS_TAG "v0.9.0" CACHE STRING "The quickjs-ng release to build against")

FetchContent_Declare(quickjs
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG ${OPENQBROWSER_QUICKJS_TAG})
FetchContent_MakeAvailable(quickjs)
```

Setting `-DOPENQBROWSER_SCRIPTING=OFF` builds the browser without it. Every
JavaScript source file is guarded by that macro, so the browser still compiles,
still renders, and reports honestly that scripts do not run.

## Files

| File | Lines | What it does |
| --- | ---: | --- |
| `ScriptEngine.h/.cpp` | 200 | The seam the browser uses, and the fallback when there is no engine |
| `Engine.h/.cpp` | 520 | Runtime lifecycle, evaluation, exception formatting, promise jobs |
| `Bindings.h/.cpp` | 545 | Wrapper cache, detached-node graveyard, prototypes, installation |
| `DomBindings.cpp` | 1290 | Node, Element and Document members |
| `ElementObjects.cpp` | 675 | `classList`, `style`, reflected attributes |
| `Events.h/.cpp` | 800 | Listener registry, the dispatch walk, event objects |
| `EventsMembers.cpp` | 190 | `Event`, `CustomEvent`, `MouseEvent`, `KeyboardEvent` constructors |
| `Globals.cpp` | 330 | `setTimeout` and friends, `console` |
| `WindowBindings.cpp` | 310 | `navigator`, `location`, `screen`, `alert`, the API stubs |
| `TimerQueue.h/.cpp` | 390 | Timer scheduling, delay clamping, the frame clock |

## One runtime per page

`Engine` owns a `JSRuntime` and a `JSContext`. One runtime per page isolates
pages from each other: a page that exhausts its heap or corrupts its own globals
cannot reach another tab. It also makes disposal a single clear operation when a
tab closes.

A page is untrusted code, so the runtime is given limits before anything runs:

```cpp
JS_SetMemoryLimit(runtime, 256 * 1024 * 1024);
JS_SetMaxStackSize(runtime, 2 * 1024 * 1024);
```

Both matter: without the stack limit, deeply recursive page script takes the
whole browser down with a native stack overflow that no `try`/`catch` can stop.

`evaluate()` compiles with `JS_EVAL_TYPE_GLOBAL`, which is what a `<script>`
element contains, and returns the script's **completion value** alongside its
messages. Draining `JS_IsJobPending` afterwards is what makes `await` and
`.then()` work for code that a synchronous evaluation started.

## Binding the DOM

Script sees ordinary JavaScript objects with accessor properties, so page code
written for a browser works unchanged:

```js
document.getElementById("x").classList.add("on");
el.textContent = "hi";
el.style.color = "red";
el.addEventListener("click", handler);
```

Every member is an accessor rather than a stored property, because reading it
must consult the live tree: `el.textContent` has to reflect the current
children, not a snapshot taken when the wrapper was created.

### Ownership

The DOM owns its nodes through `std::unique_ptr`, but script holds references
that outlive a node's removal from the tree:

```js
var el = document.getElementById("x");
el.remove();
el.textContent;      // still works
```

One rule makes that safe:

> **Every node that is not in the document tree is owned by the context's
> graveyard, for as long as the context lives.**

A node is owned by exactly one of the tree or the graveyard, never both and
never by a wrapper. Detaching a node moves it to the graveyard; inserting one
takes it back out. Nothing is freed while script could reach it, which trades a
bounded amount of memory for the absence of use-after-free. The graveyard is
emptied when the runtime is destroyed.

This replaced an earlier design that gave each wrapper an ownership flag. That
design was wrong for a case that is easy to write by accident: a node detached
by one script and inserted by another lost track of who owned it. The graveyard
has no such ambiguity, because ownership is a property of the node's position,
not of a wrapper that may or may not still exist.

### Wrappers

Each node has at most one wrapper, so `el === document.body` holds and the same
object comes back from every API, including `event.target`. Wrappers are cached
in a `QHash<dom::Node *, JSValue>` for the life of the context.

### Prototypes

Four prototypes are built once: `Node`, `Element`, `Document` and `Event`.
`Element` and `Document` inherit from `Node`, so neither repeats the node
members. The constructors are installed as globals, so `instanceof` works:

```js
document.body instanceof Element     // true
new Event("click") instanceof Event  // true
```

### What is exposed

**Node**: `nodeName`, `nodeType`, `nodeValue`, `textContent`, `parentNode`,
`parentElement`, `childNodes`, `firstChild`/`lastChild`,
`nextSibling`/`previousSibling`, `ownerDocument`, `appendChild`,
`insertBefore`, `removeChild`, `replaceChild`, `cloneNode`, `hasChildNodes`,
`contains`, `remove`, `addEventListener`, `removeEventListener`,
`dispatchEvent`, and the node type constants.

**Element**: `tagName`, `id`, `className`, `classList`, `innerHTML`,
`outerHTML`, `style`, `children`, `firstElementChild`, `childElementCount`,
sibling elements, `getAttribute`/`setAttribute`/`removeAttribute`/
`hasAttribute`, `querySelector(All)`, `getElementsByTagName`,
`getElementsByClassName`, `closest`, `matches`, `getBoundingClientRect`, and
the reflected attributes (`href`, `src`, `value`, `disabled`, and the rest).

**Document**: `getElementById`, `querySelector(All)`, `getElementsBy*`,
`createElement`, `createTextNode`, `createComment`, `write`, `documentElement`,
`head`, `body`, `title`, `URL`, `readyState`.

Two of these are deliberately shallow:

- `getBoundingClientRect` reports a zero rect. The geometry lives in the
  renderer, which the DOM bindings cannot reach without exposing the whole
  layout engine to script. Reporting a wrong number would be worse than
  reporting none.
- `getComputedStyle` reports what the `style` attribute carries, not the
  cascaded value, for the same reason.

## Events

`addEventListener`, `dispatchEvent` and the event constructors are implemented
following the DOM standard's walk:

1. The propagation path is collected from the target up to the root.
2. Capture listeners run from the root **down**, skipping the target.
3. The target's own capture listeners run, then its bubble listeners.
4. Bubble listeners run back **up** the path, for an event that bubbles.
5. The window's listeners run last.

The window is the last stop because a page registers its `DOMContentLoaded` and
`load` listeners on `window`, which is the global object rather than a node. It
therefore has its own listener table, and the lifecycle events the browser fires
consult both.

Matching `removeEventListener` is by **handler identity and capture flag**, as
in a browser: a different function with the same body does not remove anything.
Listeners registered with `{ once: true }` are retired before they run, so a
handler that re-dispatches the same event does not run twice.

Event types are case-sensitive. Lower-casing them on registration would stop a
listener for `DOMContentLoaded` from ever matching, which is exactly the bug
this implementation first had.

## Timers

`setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`,
`requestAnimationFrame` and `queueMicrotask` are implemented, with the clamping
browsers apply:

- a delay of zero or less still waits a turn, so a callback cannot run inside
  the call that scheduled it;
- a deeply nested timer is held to 4ms, so a callback that reschedules itself
  cannot starve the browser;
- a delay is capped at 24 hours, so a page cannot overflow the clock;
- an interval reschedules from its previous due time, so it does not drift.

Timers live in a `TimerQueue` that the **caller** advances:

```cpp
engine->runDueTimers(nowMs);      // run everything due at this time
```

That is what keeps a callback from firing in the middle of layout, and it is
what lets the tests drive time deterministically instead of sleeping. A callback
that throws is reported to the console and does not stop the others.

## Running a page's scripts

`Page` runs scripts the way the specification orders them:

1. **Classic scripts** run where they appear, in document order. Their changes
   are visible to every later script.
2. **`defer` and `async` scripts** run after the document is parsed. `defer`
   keeps document order, which is the ordering distinction a page can depend on.
3. **`DOMContentLoaded`** fires after the deferred scripts.
4. **`load`** fires after the subresources.
5. **Timers** run on the frame clock, after which the document is re-styled and
   re-laid out if a callback changed it.

A `<script>` whose `type` is not a JavaScript MIME type is treated as data, not
code. That matters for `<script type="application/json">`, which is common and
would be both wrong and unsafe to execute.

A script removed by an earlier script does not run, which the plan handles by
checking each element against the tree before using it.

### The load state machine

Script sources are fetched through the same `ResourceLoader` as images and
stylesheets, so a script is a subresource with its own accounting. Getting this
wrong produces a browser that hangs rather than one that fails, so it is worth
stating:

- The document is identified by a **flag**, not by its URL, because a redirect
  answers at a URL that was never requested.
- A `file://` script is read **synchronously**, so it completes while the
  document is still being built. Routing by state alone fed a script's source to
  the HTML parser; the fetch list is gathered before fetching for the same
  reason.
- The "everything is done" check runs only in the subresource phase, and a
  script fetch is counted separately from an image fetch, because a page whose
  only subresource is a script must still finish loading.

Each of those was a real bug found by the tests in `tests/integration/tst_scripting.cpp`.

## What is not supported

Stated plainly, because a page that feature-detects these should take its own
fallback rather than silently doing the wrong thing:

- **Modules.** `<script type="module">` is reported as skipped rather than run.
  Its imports would need a module loader and a fetch hook the engine does not
  have.
- **`fetch` and `XMLHttpRequest`.** Network access from script is not exposed,
  and both are **absent** rather than stubbed. That is deliberate: a stub would
  make `typeof fetch === "function"` true, so a page would take its modern path,
  call it, and break. Leaving them undefined lets feature detection pick the
  fallback. Adding them means giving script a handle on the loader and a
  promise-based completion path.
- **Web Storage.** `localStorage` and `sessionStorage` are present and empty,
  so a page reading back what it wrote gets nothing rather than an error.
- **Navigation from script.** `location.href` reads correctly, but `reload`,
  `assign` and `replace` report rather than navigate. Navigating would mean
  unwinding the evaluation that asked for it; the browser's own loader is the
  only thing that may start a load.
- **Workers, canvas, WebSocket, `postMessage`.** Absent for the same reason, so
  feature detection works.
- **Window-level calls with nothing behind them** — `window.open`, `window.print`,
  `window.scrollTo` and the rest — are present and report through the console
  when called. They are kept as callable because a page calls them
  unconditionally rather than testing for them first, so a TypeError there would
  stop a script that has nothing to do with the missing feature.
- **Selection, ranges, shadow DOM, mutation observers.** Absent.

The stubs exist so that feature detection does not throw and a page takes its
fallback path. Each reports once through the console, so the behaviour is
visible rather than mysterious.

## Testing

Two suites cover this, and both are required to pass:

- `tests/unit/tst_javascript.cpp` (39 cases) runs real scripts against a real
  parsed document: the language, the DOM bindings, events, timers, console and
  ownership.
- `tests/integration/tst_scripting.cpp` (14 cases) drives scripted pages through
  the actual load pipeline: inline, external and deferred scripts, error
  reporting, the lifecycle events, and the completion accounting above.

The tests are what found the bugs worth listing here — the synchronous-fetch
reentrancy, the redirect misidentification, the case-sensitive event types, the
`setTimeout(f, 0)` running too early, and a `QRegularExpression` being compiled
on every class-name lookup, which alone made an article page take minutes to
lay out.

## See also

- [overview.md](overview.md) for where scripting sits in the pipeline
- [browser.md](browser.md) for the load lifecycle a page's scripts run inside
- [testing.md](testing.md) for how to run the suites
