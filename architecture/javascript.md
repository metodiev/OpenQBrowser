# JavaScript

**JavaScript is not implemented.** OpenQBrowser parses, styles, lays out and
paints documents, and it does not execute a single line of page script. This
document states what the code does instead, what an engine would have to satisfy,
and why the seam exists before the engine does.

## The seam

`src/javascript/ScriptEngine.h` and `ScriptEngine.cpp` (about 140 lines) are the
entire JavaScript module.

```cpp
class ScriptEngine
{
public:
    virtual bool isAvailable() const { return false; }        // always false
    virtual ExecutionResult execute(const QString &source, dom::Document *document,
                                    const QString &sourceName = {});
    const QList<ConsoleMessage> &messages() const;
    void clearMessages();
    QStringList skippedScripts() const;
    void noteSkippedScript(const QString &description);
    static QString availabilityNote();
    bool scriptsBlockRendering() const { return !isAvailable(); }   // always true
protected:
    QList<ConsoleMessage> m_messages;
    QStringList m_skipped;
};
```

Supporting types: `ConsoleMessage` (`Level`: `Log`, `Info`, `Warning`, `Error`;
plus `text`, `source`, `line`) and `ExecutionResult` (`success`, `error`, `line`,
`messages`).

The interface takes a `dom::Document *` and a source string and returns messages,
which is the smallest contract that a real engine could satisfy. It is
deliberately virtual with a trivial default, so replacing it means adding one
subclass and one construction site in `Page`.

## What happens today

`Page::buildDocument()` (in `src/browser/Page.cpp`) walks the parsed document:

```cpp
for (dom::Element *element : m_document->getElementsByTagName("script")) {
    const QString src = element->attribute("src");
    if (!src.isEmpty())
        m_scripts->execute(QString(), m_document.get(),
                           m_document->resolveUrl(src).toString());
    else
        m_scripts->execute(element->textContent(), m_document.get(), "inline");
}
```

`ScriptEngine::execute()` then does this, in order:

1. If `isAvailable()` were true, it returns a failure explaining that the engine
   claimed to be usable without implementing execution. That branch exists so a
   subclass that forgets to override `execute()` cannot silently appear to work.
2. It counts the source lines and appends an **Info** console message:
   `Script not executed: OpenQBrowser has no JavaScript engine yet (N lines from
   <source>).`
3. It appends a **Warning**: `The page may not work as intended without scripting.
   See architecture/javascript.md for the plan.`
4. It records the script in `skippedScripts()` — `"inline script"` or the resolved
   URL.
5. It returns `success == true` with no error.

The net effect on a page:

* Scripts are **noted, reported and never run.** `<script src>` is not even
  fetched: `Page::collectSubresources()` only queues `<link rel=stylesheet>` and
  `<img>`, so a page whose only network traffic is script is fetched once, for the
  HTML.
* `<noscript>` content is rendered, because the tokenizer treats `noscript` as raw
  text and the tree thus contains no markup from it; the rest of the page renders
  as if scripting were disabled, which is the comparison a reader should make.
* A page that builds its content in script renders empty or partial. A page that
  merely enhances server-rendered HTML renders correctly.
* Nothing that depends on script works: event handlers such as `onclick`,
  `javascript:` URLs, `<button>` behaviour, form submission, timers, and every DOM
  API. `:hover` and `:active` styling does not change either, because no
  pseudo-class state is tracked at all (`css.md` lists the supported
  pseudo-classes).
* `ScriptEngine::scriptsBlockRendering()` returns `true` whenever no engine is
  available — there is nothing to wait for, so nothing is deferred.

The DevTools view makes this visible rather than mysterious.
`Inspector::scriptSummary()` prints the number of inline and external scripts in
the document, whether an engine is available, the text of
`ScriptEngine::availabilityNote()` (which points at
`src/javascript/ScriptEngine.h`), each skipped script, and every console message;
it is part of `Inspector::fullReport()`, which `openqbrowser --dump-all` prints.
`--dump-dom about:home` on a page with scripts shows the same information.

Because `ScriptEngine::execute()` returns `success == true`, `Page` does not build
an error page for a document containing scripts; the page renders.

## What a future engine would have to provide

The seam was designed so these requirements are already visible in the code that
surrounds it. An engine plugged in here would need:

1. **A parser and interpreter** for ECMAScript, plus the host objects the web
   platform defines. This is the part that does not exist and is the reason the
   feature is absent rather than partial.
2. **A DOM binding** over `oqb::dom::Node`, `Element`, `Document` and the
   attribute map in `src/dom/Node.h`. The DOM is the only object graph an engine
   would be given: `execute()` receives `dom::Document *` and nothing else.
   Today that tree is read by CSS and the box tree and never mutated after
   parsing, so a binding that can add, remove and reorder nodes would also require
   the renderer to be re-run in response.
3. **Re-entry into the pipeline.** A DOM mutation or a style change must lead to
   `StyleEngine::computeStyles()`, `BoxTreeBuilder::build()` and
   `LayoutEngine::layout()` again. `Page::buildLayout()` already does exactly that
   from scratch and can be called again; that is the integration point.
4. **Event dispatch.** Mouse and keyboard input currently ends in
   `ui::PageView`, which resolves a click to a link
   (`PageView::linkElementAt()`) and emits `linkActivated()`. An engine would need
   the events before that resolution, and a document-level target list rather than
   the single `isLink()` test.
5. **Timers and a task queue** that integrate with Qt's event loop, since the whole
   browser runs on it.
6. **A console and error channel.** This is the one part already implemented:
   `ConsoleMessage` and `ExecutionResult` are the types, and
   `Inspector::scriptSummary()` is the view.
7. **Execution ordering and blocking semantics.** `Page::buildDocument()` currently
   runs scripts after the *whole* document is parsed and before subresource
   collection. A real engine must run each classic script as the tokenizer reaches
   it — which is why the tokenizer is context-sensitive and the parser keeps a
   stack of open elements — and must suspend for `<script src>` fetches. The
   `scriptsBlockRendering()` hook is where the difference between "nothing to wait
   for" and "parser-blocking script" would be expressed.
8. **Isolation and limits.** A timeout or instruction budget per script, and
   ideally a separate execution context. `security.md` describes what isolation
   does not exist today.

`ExecutionResult::success` and `execute()`'s `sourceName` already carry enough
information for error reporting with a source name and line number, and
`ScriptEngine::m_skipped` is a ready-made place for a diagnostic list.

## Why the seam exists before the engine

Three reasons, all of which the header comment on `ScriptEngine` spells out:

* **The loader already knows where scripts are.** `Page::buildDocument()` knows the
  document order, the `src` resolution rule (`Document::resolveUrl()`, which
  honours `<base href>`) and the distinction between inline and external scripts.
  That logic does not have to be written later, and it is exercised today.
* **The reporting path is defined.** A script message reaches the user through the
  same channel as a network error or a broken image: the inspector, the console
  list and, for `Page::failed()`, the error page. Building the reporting path after
  the engine would mean retrofitting the UI at both ends.
* **The rest of the browser does not depend on it.** `scriptsBlockRendering()`
  returns `true`, `execute()` returns success, and no stage of the pipeline
  branches on `isAvailable()`. Adding an engine therefore changes one construction
  site (`Page::Page()` creates `std::make_unique<javascript::ScriptEngine>()`) and
  one subclass, not the architecture.

Until then, the behaviour is deliberately visible rather than silent: the console
says a script was skipped, the inspector lists it, and this document says why.
