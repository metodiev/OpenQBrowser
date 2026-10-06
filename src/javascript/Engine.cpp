#include "javascript/Engine.h"

#include "dom/Document.h"
#include "javascript/Bindings.h"
#include "javascript/Fetch.h"
#include "network/ScriptFetch.h"
#include "javascript/TimerQueue.h"

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::javascript {

#ifdef OPENQBROWSER_SCRIPTING

namespace detail {

/// Everything one QuickJS runtime needs, kept together so that Engine's header
/// does not have to expose the engine's C API to the rest of the browser.
struct Context
{
    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    dom::Document *document = nullptr;
    /// The value of `window`, which is also the global object.
    JSValue global = JS_UNDEFINED;
    /// The ids OpenQBrowser's wrapper classes were registered under.
    Bindings::ClassIds classes;
    TimerQueue timers;
    QList<ConsoleMessage> messages;
    /// Where fetch() sends its requests. Not owned: the loader outlives the
    /// engine, and a null provider is a valid state that fetch reports rather
    /// than crashing on.
    network::ScriptFetchProvider *fetchProvider = nullptr;
    /// The live connection from the provider's completion signal. Kept so that
    /// replacing a provider disconnects the old one: two providers both
    /// delivering would settle promises against the wrong page.
    QMetaObject::Connection fetchConnection;

    ~Context()
    {
        if (context) {
            // Everything that holds a value has to let go before the context
            // that owns those values is freed: the timers' callbacks, the
            // listeners, the wrapper cache and the detached nodes.
            timers.clear(context);
            // The fetch bindings hold resolution functions, which are values of
            // this context too, so they go before it does.
            Fetch::destroyContext(context);
            Bindings::destroy(context);
            if (!JS_IsUndefined(global))
                JS_FreeValue(context, global);
            JS_FreeContext(context);
        }
        if (runtime)
            JS_FreeRuntime(runtime);
    }
};

/// Renders a completed value as the text a console would show under it: a string
/// unquoted, null and undefined by name, and everything else through JSON, which
/// is what makes an object readable rather than "[object Object]".
QString describeValue(JSContext *ctx, JSValueConst value)
{
    if (JS_IsUndefined(value))
        return QStringLiteral("undefined");
    if (JS_IsNull(value))
        return QStringLiteral("null");
    if (JS_IsString(value))
        return QString::fromUtf8(JS_ToCString(ctx, value));

    JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    if (!JS_IsException(json) && !JS_IsUndefined(json)) {
        const QString text = QString::fromUtf8(JS_ToCString(ctx, json));
        JS_FreeValue(ctx, json);
        return text;
    }

    // A value JSON cannot represent - a function, a DOM node - falls back to its
    // string form, with any pending exception cleared so it does not surface
    // later.
    JS_FreeValue(ctx, json);
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);

    if (const char *text = JS_ToCString(ctx, value)) {
        const QString result = QString::fromUtf8(text);
        JS_FreeCString(ctx, text);
        return result;
    }
    return QStringLiteral("undefined");
}

/// Formats a thrown value the way a browser's console would: the error message,
/// then the first stack frame, so a reader can find the line at fault.
QString formatException(JSContext *ctx, JSValueConst exception)
{
    if (!JS_IsError(ctx, exception)) {
        // A thrown non-error can be anything, so it is converted as text.
        if (const char *text = JS_ToCString(ctx, exception)) {
            const QString message = QString::fromUtf8(text);
            JS_FreeCString(ctx, text);
            return message;
        }
        return QStringLiteral("uncaught exception (the value could not be converted to text)");
    }

    // An error reports its name and message, then the first stack frame. The
    // frame is what turns "not a function" into "not a function at line 3", and
    // the name is what tells a reader whether the page or the browser is at
    // fault, so both are needed.
    QString description;

    if (JSValueConst name = JS_GetPropertyStr(ctx, exception, "name");
        !JS_IsUndefined(name) && !JS_IsNull(name)) {
        if (const char *text = JS_ToCString(ctx, name)) {
            description = QString::fromUtf8(text);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, name);
    }

    QString message;
    if (JSValueConst messageValue = JS_GetPropertyStr(ctx, exception, "message");
        !JS_IsUndefined(messageValue) && !JS_IsNull(messageValue)) {
        if (const char *text = JS_ToCString(ctx, messageValue)) {
            message = QString::fromUtf8(text);
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, messageValue);
    }

    if (description.isEmpty())
        description = QStringLiteral("Error");
    if (message.isEmpty())
        message = QStringLiteral("(no message)");

    QString result = QStringLiteral("%1: %2").arg(description, message);

    // The first stack frame adds the location. QuickJS's stack text starts with
    // the frames rather than repeating the message, so the first non-empty line
    // that looks like a frame is the one to take.
    if (JSValueConst stack = JS_GetPropertyStr(ctx, exception, "stack");
        !JS_IsUndefined(stack) && !JS_IsNull(stack)) {
        if (const char *text = JS_ToCString(ctx, stack)) {
            const QStringList lines
                = QString::fromUtf8(text).split(u'\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QString trimmed = line.trimmed();
                if (trimmed.startsWith(QLatin1String("at ")) || trimmed.startsWith(QLatin1String("@"))) {
                    result += QStringLiteral("\n    ") + trimmed;
                    break;
                }
            }
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, stack);
    }

    return result;
}

/// The line number an exception carries, when it has one.
int exceptionLine(JSContext *ctx, JSValueConst exception)
{
    if (!JS_IsError(ctx, exception))
        return 0;

    JSValueConst line = JS_GetPropertyStr(ctx, exception, "lineNumber");
    int32_t value = 0;
    JS_ToInt32(ctx, &value, line);
    JS_FreeValue(ctx, line);
    return static_cast<int>(value);
}

} // namespace detail

// -------------------------------------------------------------- Engine

Engine::Engine(dom::Document *document)
    : m_context(std::make_unique<detail::Context>())
{
    m_context->runtime = JS_NewRuntime();
    if (!m_context->runtime)
        return;

    // A page is untrusted, so the engine is given a ceiling on how much it can
    // allocate and how deep it may recurse before it is stopped. Without these a
    // single line of page script can take the whole browser down.
    JS_SetMemoryLimit(m_context->runtime, 256 * 1024 * 1024);
    JS_SetMaxStackSize(m_context->runtime, 2 * 1024 * 1024);

    m_context->context = JS_NewContext(m_context->runtime);
    if (!m_context->context)
        return;

    m_context->document = document;

    // The bindings install window, document and the constructors, and return the
    // global object so that later evaluations share the same realm.
    m_context->global = Bindings::install(m_context->context, m_context->classes,
                                          m_context->document, &m_context->timers,
                                          &m_context->messages);

    // fetch() is installed here rather than by the bindings, because it needs the
    // provider the browser attaches afterwards. Installing it now means a page
    // always sees the global, and a page that finds it without a provider gets a
    // rejection that says so rather than a missing function it has to guess about.
    if (!JS_IsException(m_context->global)) {
        Fetch::install(m_context->context, m_context->global,
                       m_context->fetchProvider);
    }
}

Engine::~Engine() = default;

bool Engine::isValid() const
{
    return m_context && m_context->context != nullptr;
}

void Engine::setDocument(dom::Document *document)
{
    if (!isValid())
        return;

    // A promise from the old document must not resolve into the new one: the
    // handler would run against a tree the page no longer has, which is how a
    // stale response overwrites a freshly loaded page. They are failed instead,
    // so a page that kept the promise sees why.
    if (document != m_context->document)
        Fetch::cancelAll(m_context->context, QStringLiteral("the page was replaced"));

    m_context->document = document;
    Bindings::setDocument(m_context->context, document);
}

dom::Document *Engine::document() const
{
    return m_context ? m_context->document : nullptr;
}

TimerQueue &Engine::timers()
{
    return m_context->timers;
}

const TimerQueue &Engine::timers() const
{
    return m_context->timers;
}

bool Engine::hasPendingWork() const
{
    if (!m_context)
        return false;

    // A fetch in flight is work: the promise it will settle has a handler that
    // has not run yet. Leaving it out would let the frame clock stop while a
    // request was outstanding, so a response that arrived would settle a promise
    // whose handler never ran - the page would look frozen with no error.
    return !m_context->timers.isEmpty() || Fetch::pendingCount(m_context->context) > 0;
}

bool Engine::takeDocumentTouched()
{
    if (!isValid())
        return false;
    return Bindings::takeDocumentTouched(m_context->context);
}

JSContext *Engine::context() const
{
    return m_context ? m_context->context : nullptr;
}

void Engine::setFetchProvider(network::ScriptFetchProvider *provider)
{
    if (!isValid() || !provider)
        return;

    m_context->fetchProvider = provider;

    // The bindings snapshot nothing: the provider is handed on so that the
    // promise fetch() returns is registered with whoever will answer it.
    Fetch::setProvider(m_context->context, provider);

    // Replacing a provider is rare - a document keeps its loader - but two live
    // connections would deliver every response twice, so the old one goes first.
    QObject::disconnect(m_context->fetchConnection);

    // The response is handed to the promise that is waiting for it. Delivery goes
    // through the engine rather than to the provider, so script sees the result
    // in the same realm it made the call from.
    m_context->fetchConnection = QObject::connect(
        provider, &network::ScriptFetchProvider::fetchFinished, provider,
        [this](int requestId, const network::ScriptFetchResponse &response) {
            if (isValid())
                Fetch::deliver(m_context->context, requestId, response);
        });
}

void Engine::setReadyState(const QString &state)
{
    if (!isValid())
        return;

    // readyState lives on the global object, because it belongs to the page
    // rather than to the document tree, and the bindings read it from there.
    JSValue global = JS_GetGlobalObject(m_context->context);
    JS_DefinePropertyValueStr(m_context->context, global, "__readyState",
                              Bindings::newString(m_context->context, state), JS_PROP_C_W_E);
    JS_FreeValue(m_context->context, global);
}

ExecutionResult Engine::evaluate(const QString &source, const QString &sourceName)
{
    ExecutionResult result;

    if (!isValid()) {
        result.success = false;
        result.error = availabilityNote();
        return result;
    }

    // Scripts are compiled as global code, which is what a <script> element
    // contains. The filename shown in a stack trace is the script's URL, or a
    // description of where it came from when it is inline.
    const QByteArray utf8 = source.toUtf8();
    const QByteArray filename
        = (sourceName.isEmpty() ? QStringLiteral("<inline script>") : sourceName).toUtf8();

    const int messageCountBefore = static_cast<int>(m_context->messages.size());

    JSValue value = JS_Eval(m_context->context, utf8.constData(),
                            static_cast<size_t>(utf8.size()), filename.constData(),
                            JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(m_context->context);
        result.success = false;
        result.error = detail::formatException(m_context->context, exception);
        result.line = detail::exceptionLine(m_context->context, exception);
        JS_FreeValue(m_context->context, exception);
    } else {
        // The script's completion value: the value of its last statement. It is
        // what a console prints under an expression, and reading it here is the
        // only point at which it is still available.
        result.value = detail::describeValue(m_context->context, value);
    }

    JS_FreeValue(m_context->context, value);

    // The context holds one promise job queue; draining it here runs the
    // handlers of any promise that settled during the script, which is what
    // makes `await` and `.then()` work for code that runs synchronously.
    JSContext *jobContext = nullptr;
    while (JS_IsJobPending(m_context->runtime)) {
        if (JS_ExecutePendingJob(m_context->runtime, &jobContext) < 0) {
            if (jobContext) {
                JSValue exception = JS_GetException(jobContext);
                result.success = false;
                result.error = detail::formatException(jobContext, exception);
                JS_FreeValue(jobContext, exception);
            }
            break;
        }
    }

    // Anything the script logged through console is reported back with the
    // result, so the caller can surface it without polling. Only the messages
    // added since this call started are taken, since the log is append-only and
    // an earlier call has already reported the rest.
    for (int i = messageCountBefore; i < m_context->messages.size(); ++i)
        result.messages.append(m_context->messages.at(i));

    // A message with no source of its own is attributed to the script that
    // produced it.
    for (ConsoleMessage &message : result.messages) {
        if (message.source.isEmpty())
            message.source = sourceName;
    }

    return result;
}

const QList<ConsoleMessage> &Engine::messages() const
{
    static const QList<ConsoleMessage> kEmpty;
    return m_context ? m_context->messages : kEmpty;
}

void Engine::clearMessages()
{
    if (m_context)
        m_context->messages.clear();
}

void Engine::runDueTimers(qint64 nowMs)
{
    if (!isValid())
        return;

    m_context->timers.runDue(m_context->context, nowMs);

    // A timer callback may schedule more work, so the job queue is drained
    // again: a promise resolved inside a timeout would otherwise not settle
    // until the next script ran.
    JSContext *jobContext = nullptr;
    while (JS_IsJobPending(m_context->runtime)) {
        if (JS_ExecutePendingJob(m_context->runtime, &jobContext) < 0) {
            if (jobContext) {
                JSValue exception = JS_GetException(jobContext);
                m_context->messages.append({ConsoleMessage::Level::Error,
                                            detail::formatException(jobContext, exception),
                                            QStringLiteral("timer"), 0});
                JS_FreeValue(jobContext, exception);
            }
            break;
        }
    }
}



bool Engine::isSupported()
{
    return true;
}

QString Engine::engineVersion()
{
    // quickjs-ng exposes the version as three numbers rather than one string.
    return QStringLiteral("QuickJS %1.%2.%3")
        .arg(QJS_VERSION_MAJOR)
        .arg(QJS_VERSION_MINOR)
        .arg(QJS_VERSION_PATCH);
}

QString Engine::availabilityNote()
{
    return QStringLiteral(
        "Scripts run on the embedded QuickJS engine. The DOM is bound into it, "
        "so scripts can read and modify the document, listen for events and use "
        "timers. Not implemented: modules, workers, storage APIs and most of the "
        "network APIs.");
}

#else // no scripting

// A build without QuickJS keeps the same interface and reports why nothing runs,
// so the rest of the browser does not need to know which build it is in.

namespace detail {
struct Context
{
};
} // namespace detail

Engine::Engine(dom::Document *document)
    : m_context(std::make_unique<detail::Context>())
{
    Q_UNUSED(document);
}

Engine::~Engine() = default;

bool Engine::isValid() const
{
    return false;
}

void Engine::setDocument(dom::Document *document)
{
    Q_UNUSED(document);
}

dom::Document *Engine::document() const
{
    return nullptr;
}

TimerQueue &Engine::timers()
{
    static TimerQueue kNone;
    return kNone;
}

const TimerQueue &Engine::timers() const
{
    static const TimerQueue kNone;
    return kNone;
}

bool Engine::hasPendingWork() const
{
    return false;
}

ExecutionResult Engine::evaluate(const QString &source, const QString &sourceName)
{
    ExecutionResult result;
    result.success = false;
    result.error = availabilityNote();

    const int lineCount = source.count(u'\n') + 1;
    result.messages.append({ConsoleMessage::Level::Info,
                            QStringLiteral("Script not executed: this build has no JavaScript "
                                           "engine (%1 line%2 from %3).")
                                .arg(lineCount)
                                .arg(lineCount == 1 ? QString() : QStringLiteral("s"))
                                .arg(sourceName.isEmpty() ? QStringLiteral("an inline script")
                                                          : sourceName),
                            sourceName.isEmpty() ? QStringLiteral("inline") : sourceName, 0});
    return result;
}

const QList<ConsoleMessage> &Engine::messages() const
{
    static const QList<ConsoleMessage> kEmpty;
    return kEmpty;
}

void Engine::runDueTimers(qint64 nowMs)
{
    Q_UNUSED(nowMs);
}

void Engine::clearMessages()
{
}

bool Engine::takeDocumentTouched()
{
    // Nothing can change the document without an engine, so the browser never
    // has to re-lay out on a script's account.
    return false;
}



bool Engine::isSupported()
{
    return false;
}

QString Engine::engineVersion()
{
    return QStringLiteral("none");
}

QString Engine::availabilityNote()
{
    return QStringLiteral(
        "OpenQBrowser was built without a JavaScript engine, so page script does "
        "not run. Configure with -DOPENQBROWSER_SCRIPTING=ON to enable it.");
}

#endif // OPENQBROWSER_SCRIPTING

// -------------------------------------------------- QuickJsScriptEngine

QuickJsScriptEngine::QuickJsScriptEngine(dom::Document *document)
    : m_engine(std::make_unique<Engine>(document))
{
}

QuickJsScriptEngine::~QuickJsScriptEngine() = default;

bool QuickJsScriptEngine::isAvailable() const
{
    return m_engine && m_engine->isValid();
}

void QuickJsScriptEngine::setDocument(dom::Document *document)
{
    if (m_engine && document && document != m_engine->document())
        m_engine->setDocument(document);
}

void QuickJsScriptEngine::clearMessages()
{
    ScriptEngine::clearMessages();
    if (m_engine)
        m_engine->clearMessages();
}

ExecutionResult QuickJsScriptEngine::execute(const QString &source, dom::Document *document,
                                             const QString &sourceName)
{
    if (!m_engine)
        return ScriptEngine::execute(source, document, sourceName);

    // A new document means the bindings have to point at it; this happens when a
    // page navigates without the engine being recreated.
    if (document && document != m_engine->document())
        m_engine->setDocument(document);

    ExecutionResult result = m_engine->evaluate(source, sourceName);

    for (const ConsoleMessage &message : result.messages)
        m_messages.append(message);

    if (!result.success) {
        // A failing script is reported on the console the way a browser reports
        // it, with the location. Recording it only as "skipped" would lose the
        // reason, and the console is where a reader looks for it.
        ConsoleMessage message;
        message.level = ConsoleMessage::Level::Error;
        message.text = result.error;
        message.source = sourceName.isEmpty() ? QStringLiteral("inline script") : sourceName;
        message.line = result.line;

        // The engine's own messages are the page's console output; this one is
        // the browser's report about a script that failed, so it is appended to
        // both the result and the log the panel shows.
        result.messages.append(message);
        m_messages.append(message);
    }

    return result;
}

} // namespace oqb::javascript
