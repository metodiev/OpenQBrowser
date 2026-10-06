#include "javascript/BindingsInternal.h"

#include "javascript/TimerQueue.h"

#include <QDateTime>

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript::detail {
namespace {

/// A JSON value that a timer callback receives as its extra argument. The timer
/// queue owns the text because it has to outlive the call that scheduled it.
QString encodeArgument(JSContext *context, JSValueConst value)
{
    if (JS_IsUndefined(value))
        return {};

    JSValue json = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);

    // A value JSON cannot represent - a function, a cyclic object - is passed
    // through as its string form rather than failing the call, which is what a
    // browser does when it cannot clone the argument.
    if (JS_IsException(json) || JS_IsUndefined(json)) {
        JS_FreeValue(context, json);
        return QStringLiteral("\"%1\"").arg(stringValue(context, value).replace(u'"', QString()));
    }

    const QString text = stringValue(context, json);
    JS_FreeValue(context, json);
    return text;
}

/// The timers the page scheduled. Shared by setTimeout, setInterval and their
/// clear functions, which is why they live together.
TimerQueue *timerQueueOf(JSContext *context)
{
    const State *state = stateFor(context);
    return state ? state->timers : nullptr;
}

// --------------------------------------------------------------- timers

JSValue setTimeoutImpl(JSContext *context, JSValueConst, int argc, JSValueConst *argv, bool repeating)
{
    if (argc < 1 || !JS_IsFunction(context, argv[0])) {
        // A non-callable first argument is accepted silently by browsers, and
        // pages rely on `setTimeout(code, 0)` with a string being ignored rather
        // than throwing.
        return JS_NewInt32(context, 0);
    }

    TimerQueue *timers = timerQueueOf(context);
    if (!timers)
        return JS_NewInt32(context, 0);

    const double delay = argc > 1 ? static_cast<double>(intValue(context, argv[1])) : 0.0;
    const QString argument = argc > 2 ? encodeArgument(context, argv[2]) : QString();

    // The queue takes its own reference, so the caller's value is left alone.
    const int id = timers->schedule(context, JS_DupValue(context, argv[0]), delay, repeating,
                                    argument, argc > 2);
    return JS_NewInt32(context, id);
}

JSValue setTimeoutFunction(JSContext *context, JSValueConst thisValue, int argc,
                           JSValueConst *argv, int magic)
{
    return setTimeoutImpl(context, thisValue, argc, argv, magic != 0);
}

JSValue clearTimeoutFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    TimerQueue *timers = timerQueueOf(context);
    if (!timers || argc < 1)
        return JS_UNDEFINED;

    timers->cancel(context, intValue(context, argv[0]));
    return JS_UNDEFINED;
}

JSValue requestAnimationFrameFunction(JSContext *context, JSValueConst, int argc,
                                      JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(context, argv[0]))
        return JS_NewInt32(context, 0);

    TimerQueue *timers = timerQueueOf(context);
    if (!timers)
        return JS_NewInt32(context, 0);

    return JS_NewInt32(context, timers->scheduleAnimationFrame(context, JS_DupValue(context, argv[0])));
}

JSValue queueMicrotaskFunction(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(context, argv[0]))
        return JS_UNDEFINED;

    // A microtask runs before any timer, so it is scheduled at delay 0 and the
    // engine drains jobs around it. A real microtask queue would need a hook the
    // engine's host loop does not yet have.
    TimerQueue *timers = timerQueueOf(context);
    if (timers)
        timers->schedule(context, JS_DupValue(context, argv[0]), 0.0, false, QString(), false);

    return JS_UNDEFINED;
}

// -------------------------------------------------------------- console

/// The console level a member name maps to.
ConsoleMessage::Level levelForMethod(const QString &method)
{
    if (method == QLatin1String("warn"))
        return ConsoleMessage::Level::Warning;
    if (method == QLatin1String("error"))
        return ConsoleMessage::Level::Error;
    if (method == QLatin1String("info") || method == QLatin1String("debug"))
        return ConsoleMessage::Level::Info;
    return ConsoleMessage::Level::Log;
}

/// Formats one console argument the way a browser's console shows it: strings
/// unquoted, everything else through JSON, and a value JSON cannot represent
/// through its string form.
QString formatArgument(JSContext *context, JSValueConst value)
{
    if (JS_IsString(value))
        return stringValue(context, value);

    if (JS_IsNull(value))
        return QStringLiteral("null");
    if (JS_IsUndefined(value))
        return QStringLiteral("undefined");

    if (JS_IsObject(value)) {
        JSValue json = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json) && !JS_IsUndefined(json)) {
            const QString text = stringValue(context, json);
            JS_FreeValue(context, json);
            return text;
        }
        JS_FreeValue(context, json);
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);

        // An element logged as-is is shown by name rather than as a serialised
        // tree, which is close to what a console does and much more readable.
        if (dom::Node *node = Bindings::nodeOf(value))
            return node->describe();

        return QStringLiteral("[object Object]");
    }

    return stringValue(context, value);
}

JSValue consoleMethod(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic)
{
    static const char *const kNames[] = {"log", "info", "warn", "error", "debug"};

    QStringList parts;
    for (int i = 0; i < argc; ++i)
        parts.append(formatArgument(context, argv[i]));

    State *state = stateFor(context);
    if (state && state->messages) {
        ConsoleMessage message;
        message.level = levelForMethod(QString::fromLatin1(kNames[magic]));
        message.text = parts.join(u' ');
        state->messages->append(message);
    }

    return JS_UNDEFINED;
}

JSValue consoleAssert(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    if (argc > 0 && boolValue(context, argv[0]))
        return JS_UNDEFINED;
    return consoleMethod(context, thisValue, argc > 0 ? argc - 1 : 0,
                         argc > 0 ? argv + 1 : nullptr, 3);
}

JSValue consoleCount(JSContext *context, JSValueConst, int argc, JSValueConst *argv)
{
    // The counters live on the console object rather than in the state, so they
    // are visible to script that inspects the object and need no separate map.
    const QString label = argc > 0 ? stringValue(context, argv[0]) : QStringLiteral("default");

    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_GetPropertyStr(context, global, "console");
    JS_FreeValue(context, global);

    if (JS_IsObject(console)) {
        JSValue counters = JS_GetPropertyStr(context, console, "__counts");
        if (JS_IsUndefined(counters)) {
            JS_FreeValue(context, counters);
            counters = JS_NewObject(context);
            JS_DefinePropertyValueStr(context, console, "__counts", JS_DupValue(context, counters),
                                      JS_PROP_C_W_E);
        }

        const QByteArray key = label.toUtf8();
        JSValue previous = JS_GetPropertyStr(context, counters, key.constData());
        const int next = intValue(context, previous) + 1;
        JS_FreeValue(context, previous);

        JS_DefinePropertyValueStr(context, counters, key.constData(), JS_NewInt32(context, next),
                                  JS_PROP_C_W_E);
        JS_FreeValue(context, counters);
    }
    JS_FreeValue(context, console);

    return JS_UNDEFINED;
}

JSValue consoleTime(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv,
                    int magic)
{
    // The label is what identifies the pair, and an absent one is "default", as
    // the console API specifies.
    Q_UNUSED(magic);
    Q_UNUSED(thisValue);

    const QString label = argc > 0 ? stringValue(context, argv[0]) : QStringLiteral("default");

    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_GetPropertyStr(context, global, "console");
    JS_FreeValue(context, global);

    if (!JS_IsObject(console)) {
        JS_FreeValue(context, console);
        return JS_UNDEFINED;
    }

    JSValue timers = JS_GetPropertyStr(context, console, "__times");
    if (JS_IsUndefined(timers)) {
        JS_FreeValue(context, timers);
        timers = JS_NewObject(context);
        JS_DefinePropertyValueStr(context, console, "__times", JS_DupValue(context, timers),
                                  JS_PROP_C_W_E);
    }

    const QByteArray key = label.toUtf8();

    if (magic == 1) {
        // console.timeEnd reports the elapsed milliseconds, which is the whole
        // point of the pair.
        JSValue startedValue = JS_GetPropertyStr(context, timers, key.constData());
        const qint64 started = intValue(context, startedValue);
        JS_FreeValue(context, startedValue);

        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - started;

        State *state = stateFor(context);
        if (state && state->messages) {
            ConsoleMessage message;
            message.level = ConsoleMessage::Level::Info;
            message.text = QStringLiteral("%1: %2ms").arg(label).arg(elapsed);
            state->messages->append(message);
        }
    } else {
        JS_DefinePropertyValueStr(context, timers, key.constData(),
                                  JS_NewInt64(context, QDateTime::currentMSecsSinceEpoch()),
                                  JS_PROP_C_W_E);
    }

    JS_FreeValue(context, timers);
    JS_FreeValue(context, console);
    return JS_UNDEFINED;
}

/// Installs the console methods on the console object.
void installConsoleMembers(JSContext *context, JSValue console)
{
    static const char *const kNames[] = {"log", "info", "warn", "error", "debug"};
    for (int i = 0; i < 5; ++i) {
        defineMagicMethod(context, console, kNames[i], consoleMethod, 0, i);
    }

    defineMethod(context, console, "assert", consoleAssert, 1);
    defineMethod(context, console, "count", consoleCount, 0);
    defineMagicMethod(context, console, "time", consoleTime, 0, 0);
    defineMagicMethod(context, console, "timeEnd", consoleTime, 0, 1);

    // The members a page calls that this engine has no notion of: a stack trace,
    // a table, indentation. They log their arguments like console.log, so a page
    // that uses them still reports what it wanted to say rather than throwing.
    for (const char *name : {"trace", "dir", "table", "group", "groupEnd", "clear"})
        defineMagicMethod(context, console, name, consoleMethod, 0, 0);
}

} // namespace

void installConsole(JSContext *context, JSValue global)
{
    JSValue console = JS_NewObject(context);
    installConsoleMembers(context, console);
    JS_DefinePropertyValueStr(context, global, "console", console, JS_PROP_C_W_E);
}

void installTimerMembers(JSContext *context, JSValue global)
{
    defineMagicMethod(context, global, "setTimeout", setTimeoutFunction, 2, 0);
    defineMagicMethod(context, global, "setInterval", setTimeoutFunction, 2, 1);
    defineMethod(context, global, "clearTimeout", clearTimeoutFunction, 1);
    // Browsers accept either id in either clear function, and pages rely on it,
    // so both are the same implementation.
    defineMethod(context, global, "clearInterval", clearTimeoutFunction, 1);
    defineMethod(context, global, "requestAnimationFrame", requestAnimationFrameFunction, 1);
    defineMethod(context, global, "cancelAnimationFrame", clearTimeoutFunction, 1);
    defineMethod(context, global, "queueMicrotask", queueMicrotaskFunction, 1);
}

} // namespace oqb::javascript::detail

#endif // OPENQBROWSER_SCRIPTING
