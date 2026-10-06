#pragma once

#include <QString>

#include <memory>

#include "javascript/ScriptEngine.h"

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::dom {
class Document;
}

namespace oqb::network {
class ScriptFetchProvider;
}

namespace oqb::javascript {

class TimerQueue;
namespace detail {
struct Context;
}

/// The JavaScript engine: a QuickJS runtime with OpenQBrowser's DOM bound into it.
///
/// One engine is created per page. A QuickJS runtime is not thread safe and holds
/// the whole script heap, so a runtime per page both isolates pages from each
/// other and makes disposal a single clear operation when a page goes away.
///
/// The engine owns three things the rest of the browser asks it for:
///
///   * `evaluate()` runs a script and reports what it logged or threw;
///   * `timers()` drives setTimeout, setInterval and requestAnimationFrame off
///     the host event loop, so a callback runs between frames rather than
///     blocking the one that scheduled it;
///   * `hasPendingWork()` tells the caller whether the page is still doing
///     something, which is what keeps the browser from considering a page
///     finished while a script is mid-flight.
class Engine
{
public:
    /// Creates a runtime and binds the browser's objects into it. A null
    /// `document` produces an engine with only the language built-ins, which the
    /// tests use.
    explicit Engine(dom::Document *document = nullptr);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /// True when a runtime exists. A failure to create one is reported through
    /// `availabilityNote()`.
    bool isValid() const;

    /// Runs `source` with `sourceName` used in stack traces and error messages.
    /// A thrown exception is reported as a failed result, never as a crash.
    ExecutionResult evaluate(const QString &source, const QString &sourceName = {});

    /// The console messages the script produced, in order.
    const QList<ConsoleMessage> &messages() const;

    /// Forgets every message the engine has collected.
    void clearMessages();

    /// Points fetch() at the loader. A null provider is refused rather than
    /// remembered: without one there is nothing to fetch from, and a page that
    /// called fetch would wait for a response that could never come.
    void setFetchProvider(network::ScriptFetchProvider *provider);

    /// Timers scheduled by the script.
    TimerQueue &timers();
    const TimerQueue &timers() const;

    /// True when a timer is due or a callback is queued, so the caller can keep
    /// servicing the page.
    bool hasPendingWork() const;

    /// True when script changed the document since this was last asked. The
    /// browser uses it to decide whether the page has to be re-styled and
    /// re-laid out before it is painted again, which is what makes a script that
    /// appends an element actually appear.
    bool takeDocumentTouched();

#ifdef OPENQBROWSER_SCRIPTING
    /// The QuickJS context, for the browser code that fires lifecycle events.
    /// Null in a build without an engine.
    JSContext *context() const;

    /// Sets document.readyState, which a page reads to decide whether the
    /// document is complete enough to query.
    void setReadyState(const QString &state);
#endif

    /// Replaces the document the bindings point at. Used when a page navigates
    /// away: the runtime is kept and the DOM objects are rebuilt for the new
    /// document, which avoids recompiling the built-ins.
    void setDocument(dom::Document *document);

    /// The document the bindings currently expose.
    dom::Document *document() const;

    /// Runs any callbacks that are due according to `nowMs`. The caller decides
    /// when, which is what lets the tests drive time deterministically.
    void runDueTimers(qint64 nowMs);

    /// True when `OPENQBROWSER_SCRIPTING` was defined and a runtime was created.
    static bool isSupported();

    /// A description of the engine's state, for the inspector and the console.
    static QString availabilityNote();

    /// The QuickJS version string, shown in about:version.
    static QString engineVersion();

private:
    std::unique_ptr<detail::Context> m_context;
};

/// The concrete ScriptEngine the browser constructs: it owns an Engine and
/// presents the ScriptEngine interface to the rest of the code.
class QuickJsScriptEngine : public ScriptEngine
{
public:
    explicit QuickJsScriptEngine(dom::Document *document = nullptr);
    ~QuickJsScriptEngine() override;

    bool isAvailable() const override;
    ExecutionResult execute(const QString &source, dom::Document *document,
                            const QString &sourceName) override;

    /// Points the engine at a document, which the browser calls once the
    /// document is parsed.
    void setDocument(dom::Document *document) override;

    /// Clears both this object's list and the engine's own log.
    void clearMessages() override;

    /// The underlying engine, for callers that need the timers or the bindings.
    Engine *engine() { return m_engine.get(); }
    const Engine *engine() const { return m_engine.get(); }

private:
    std::unique_ptr<Engine> m_engine;
};

} // namespace oqb::javascript
