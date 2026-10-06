# Developer tools

OpenQBrowser has a DevTools panel: a console that runs JavaScript in the page, an
element tree with the computed style and the rules that produced it, a layout
report and a resource list. It is the part of the browser that answers "why does
it look like that?".

The design has one rule that shapes everything: **the panel holds no state of its
own beyond the selection.** Every view is built by asking the page what it
currently is, so a refresh after a navigation, a load or a console expression
shows what is there now rather than what was there when the panel opened.

## Two layers

| Layer | Files | What it does |
| --- | --- | --- |
| The inspector | `src/devtools/Inspector.{h,cpp}` | Builds every view as text, from plain data. No Qt widgets, no browser. |
| The panel | `src/ui/DevToolsPanel.{h,cpp}` | Qt widgets around the inspector, plus the console, which acts rather than reports. |

The split is deliberate and worth keeping. `Inspector` is a pure function of the
browser's state: given a document, a style engine, a box tree and a network log,
it returns text. That makes it usable from the command line — `--dump-all` prints
exactly what the panel shows — and it means the interesting logic is testable
without a window.

`DevToolsPanel` does only what a widget has to: lay the views out, keep them in
step with the page, and forward the console's input to the engine.

## The console

The console is the one part that acts. An expression typed into it is evaluated
in the page's own engine, so it sees the same globals, the same DOM and the same
timers the page does:

```js
document.getElementById("card").classList.add("selected")
document.querySelectorAll("script").length
document.title = "changed from the console"
```

Three things make it behave like a real console:

* **The completion value is echoed.** Typing `document.title` prints the title,
  which is what makes the console usable for interrogating a page rather than
  only for running statements.
* **A failure is reported, not swallowed.** The error text carries the exception's
  name, message and first stack frame, so a reader can find the line at fault.
* **A change to the page is shown.** After evaluating, the panel calls
  `Page::serviceScripts()`, which re-styles and re-lays out when the document was
  touched, and rebuilds its own views. Without that the console would report a
  change the window did not display.

The view is **redrawn from the engine's log** on every refresh rather than
appended to. A running count of shown messages cannot be kept in step, because
the browser clears the log when a load starts — a clear arriving between two
refreshes leaves the count ahead of the list and hides the new page's first
messages. Redrawing is cheap: a page logs a handful of lines.

## The element tree

The tree is built from the document, labelled with the tag, id and classes an
author uses to tell elements apart (`div#card.badge`), and a text-only element
shows its text so a tree of divs is readable.

Selecting an element fills two views:

* **Computed style** — every property the cascade produced.
* **Applied rules** — the declarations that applied, in cascade order, each with
  the selector that carried it. This is the view that makes a surprising style
  explicable: it shows which rule won and what it was written as.

> **A bug worth recording.** The applied-rules view had *never* worked. The
> cascade built the list of applied declarations, used it, and threw it away:
> `m_applied` was declared, cleared on every style pass, read by
> `declarationsFor()`, and never written. The view reported "no declarations
> matched this element" for every element on every page. It was found by a test
> that asserted the view names the selector that set a colour.

## Picking an element

The picker arms a mode on the page view: the cursor becomes a crosshair and the
next click selects an element instead of following a link. The click is resolved
against the box tree, so what is selected is the element that actually generated
the box under the pointer — the same walk that makes a link clickable.

The mode disarms itself on the click that uses it, so it cannot be left armed by
accident.

## Keeping in step with the page

The panel has to notice four different things, and only one of them is obvious:

| What changed | How it is noticed |
| --- | --- |
| A navigation | `Page::navigationId()` changed |
| A script changed the DOM | The caller says so, or a console expression sets a flag |
| New console output | The message count changed |
| The window switched tabs | `setTab()` |

**The navigation case needed a counter, not a pointer.** The obvious check —
"is this document pointer different from the one I built the tree for?" — is
wrong, because `Page` frees the old document and the allocator hands the new one
the same address. Comparing pointers therefore reports *same document* after a
navigation, and a stale tree survives a page change. `navigationId()` increments
on every load and never repeats, which is what the panel compares.

The element tree is also rebuilt when a script may have altered the document,
because a script can insert or remove elements without changing the document
object at all.

## Tests

`tests/browser/tst_devtools.cpp` covers the panel in thirteen cases, driven
through a real window and a real load: the tree's shape and labels, selecting an
element and reading its style and rules, evaluating expressions and seeing their
values, reporting errors, reading the page's DOM from the console, showing the
page's own `console.log` output, the layout summary, the resource list, following
a navigation, picking an element, and surviving the element it was showing being
removed by a script.

The panel's widgets carry object names (`consoleOutput`, `consoleInput`,
`elementTree`, `computedStyle`, `appliedRules`, `layoutReport`,
`resourceReport`), which is what lets a test address a specific view rather than
whichever `findChild` returns first. Two of the failures those tests caught were
of exactly that kind: a test reading the network view while believing it read the
console.

## See also

- [javascript.md](javascript.md) for the engine the console evaluates in
- [rendering.md](rendering.md) for the box tree the picker walks
- [testing.md](testing.md) for how to run the suites
