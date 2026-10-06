#include "javascript/Events.h"

#include "javascript/Bindings.h"
#include "javascript/BindingsInternal.h"

#include <QSet>

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript {

namespace {

// ------------------------------------------------------------------ registry

/// A listener registered by script.
struct Listener
{
    /// The handler. Owned by the registry; released on removal or at teardown.
    JSValue callback = JS_UNDEFINED;
    /// True when the listener asked to run during the capture phase.
    bool capture = false;
    /// True when registered with `{ once: true }`, so it is dropped after one run.
    bool once = false;
    /// True when the handler is an object with a handleEvent method rather than
    /// a function.
    bool isObject = false;
};

using ListenerMap = QHash<QString, QList<Listener>>;

/// What an event object carries. Held in an opaque block so the accessors do
/// not have to read properties off the object on every access.
///
/// `target` and `currentTarget` are raw node pointers rather than JSValues. That
/// is safe because a node reachable from script never dies while the context
/// lives: detaching one moves it to the graveyard, which is emptied only when
/// the runtime is being torn down, at which point the event objects go with it.
/// Storing raw pointers also lets the wrapper cache hand back one object per
/// node, so `event.target === el` holds as it does in a browser.
struct EventData
{
    QString type;
    bool bubbles = false;
    bool cancelable = false;
    bool defaultPrevented = false;
    bool propagationStopped = false;
    bool immediatePropagationStopped = false;
    /// 0 none, 1 capture, 2 at target, 3 bubble.
    int eventPhase = 0;
    dom::Node *target = nullptr;
    dom::Node *currentTarget = nullptr;
};

/// The listener tables, one per context, because two pages in two tabs have
/// separate listener sets and one page's teardown must not disturb another's.
QHash<JSContext *, QHash<dom::Node *, ListenerMap>> &registries()
{
    static QHash<JSContext *, QHash<dom::Node *, ListenerMap>> table;
    return table;
}

QHash<dom::Node *, ListenerMap> *registryFor(JSContext *context)
{
    const auto it = registries().find(context);
    return it == registries().end() ? nullptr : &it.value();
}

/// Listeners registered on the page's window. The Window is not a node, so it
/// cannot share the node table; a page puts its DOMContentLoaded and load
/// listeners here, and the lifecycle events reach them at the end of the walk.
QHash<JSContext *, ListenerMap> &windowListeners()
{
    static QHash<JSContext *, ListenerMap> table;
    return table;
}

// -------------------------------------------------------------------- helpers

EventData *eventDataOf(JSValueConst value)
{
    return static_cast<EventData *>(detail::opaqueOf(value));
}

void finalizeEvent(JSRuntime *runtime, JSValueConst value)
{
    Q_UNUSED(runtime);
    // Nothing but plain data is held, so the finalizer has no values to release
    // and needs no context, which a finalizer does not have.
    delete eventDataOf(value);
}

void releaseListener(JSContext *context, Listener &listener)
{
    if (!JS_IsUndefined(listener.callback))
        JS_FreeValue(context, listener.callback);
    listener.callback = JS_UNDEFINED;
}

/// Identity comparison, which is what removeEventListener matches on: two
/// closures with identical source are still different listeners.
bool sameCallback(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/// Removes one specific registration. Used by removeEventListener and by a
/// once-listener retiring itself.
void removeSpecificListener(JSContext *context, dom::Node *node, const QString &type,
                            JSValueConst callback, bool capture)
{
    QHash<dom::Node *, ListenerMap> *registry = registryFor(context);
    if (!registry)
        return;

    const auto nodeEntry = registry->find(node);
    if (nodeEntry == registry->end())
        return;

    const auto typeEntry = nodeEntry->find(type);
    if (typeEntry == nodeEntry->end())
        return;

    for (int i = static_cast<int>(typeEntry->size()) - 1; i >= 0; --i) {
        Listener &listener = (*typeEntry)[i];
        if (listener.capture != capture || !sameCallback(listener.callback, callback))
            continue;
        releaseListener(context, listener);
        typeEntry->removeAt(i);
        break; // one registration per call, matching the standard
    }

    if (typeEntry->isEmpty())
        nodeEntry->remove(type);
    if (nodeEntry->isEmpty())
        registry->remove(node);
}

bool listenerFromArgument(JSContext *context, JSValueConst value, Listener *out)
{
    if (JS_IsFunction(context, value)) {
        out->callback = JS_DupValue(context, value);
        out->isObject = false;
        return true;
    }

    // An object with a callable handleEvent is the older, still supported form.
    if (JS_IsObject(value) && !JS_IsNull(value)) {
        JSValue handler = JS_GetPropertyStr(context, value, "handleEvent");
        const bool callable = JS_IsFunction(context, handler);
        JS_FreeValue(context, handler);
        if (callable) {
            out->callback = JS_DupValue(context, value);
            out->isObject = true;
            return true;
        }
    }

    return false;
}

/// Reads the third argument, which is either a capture flag or an options
/// object. Both forms appear in real pages, so both are accepted.
void readListenerOptions(JSContext *context, JSValueConst value, bool *capture, bool *once)
{
    *capture = false;
    *once = false;

    if (JS_IsBool(value)) {
        *capture = JS_ToBool(context, value) > 0;
        return;
    }

    if (!JS_IsObject(value) || JS_IsNull(value))
        return;

    JSValue captureValue = JS_GetPropertyStr(context, value, "capture");
    if (!JS_IsUndefined(captureValue))
        *capture = JS_ToBool(context, captureValue) > 0;
    JS_FreeValue(context, captureValue);

    JSValue onceValue = JS_GetPropertyStr(context, value, "once");
    if (!JS_IsUndefined(onceValue))
        *once = JS_ToBool(context, onceValue) > 0;
    JS_FreeValue(context, onceValue);
}

void invokeListener(JSContext *context, const Listener &listener, JSValueConst event)
{
    // An object handler is called as `obj.handleEvent(event)` with `obj` as the
    // receiver, which is what lets the handler keep its own state.
    JSValue function = listener.callback;
    JSValue receiver = JS_UNDEFINED;

    if (listener.isObject) {
        function = JS_GetPropertyStr(context, listener.callback, "handleEvent");
        if (!JS_IsFunction(context, function)) {
            JS_FreeValue(context, function);
            return;
        }
        receiver = listener.callback;
    }

    JSValue result = JS_Call(context, function, receiver, 1, &event);

    if (JS_IsException(result)) {
        // One failing listener does not stop the others, matching a browser: the
        // error is reported and the dispatch continues.
        JSValue exception = JS_GetException(context);
        if (const char *text = JS_ToCString(context, exception)) {
            detail::reportMessage(context, ConsoleMessage::Level::Error,
                                  QStringLiteral("Uncaught in listener: %1")
                                      .arg(QString::fromUtf8(text)));
            JS_FreeCString(context, text);
        }
        JS_FreeValue(context, exception);
    }

    JS_FreeValue(context, result);
    if (listener.isObject)
        JS_FreeValue(context, function);
}

/// Runs a copied listener list. Shared by the node and window paths so that the
/// once-handling and reference counting are written once.
bool runListenersFrom(JSContext *context, const QList<Listener> &source, dom::Node *node,
                      JSValueConst event, bool capturePhase);

/// Runs the listeners registered on one node for one event type in one phase.
/// Returns false when propagation was stopped.
bool runListenersOn(JSContext *context, dom::Node *node, JSValueConst event, bool capturePhase)
{
    auto *data = eventDataOf(event);
    if (!data)
        return true;

    QHash<dom::Node *, ListenerMap> *registry = registryFor(context);
    if (!registry)
        return true;

    const auto nodeEntry = registry->find(node);
    if (nodeEntry == registry->end())
        return true;

    const auto typeEntry = nodeEntry->find(data->type);
    if (typeEntry == nodeEntry->end())
        return true;

    // The list is copied, because a listener may add or remove listeners and
    // mutating the list being walked would skip entries. Each copy takes a
    // reference of its own, so a listener removed mid-dispatch cannot leave the
    // copy holding a freed value.
    return runListenersFrom(context, *typeEntry, node, event, capturePhase);
}

/// Runs a copied listener list. Shared by the node and window paths so that the
/// once-handling and reference counting are written once.
bool runListenersFrom(JSContext *context, const QList<Listener> &source, dom::Node *node,
                      JSValueConst event, bool capturePhase)
{
    auto *data = eventDataOf(event);
    if (!data)
        return true;

    QList<Listener> listeners;
    listeners.reserve(source.size());
    for (const Listener &listener : source) {
        Listener copy;
        copy.callback = JS_DupValue(context, listener.callback);
        copy.capture = listener.capture;
        copy.once = listener.once;
        copy.isObject = listener.isObject;
        listeners.append(copy);
    }

    dom::Node *previousTarget = data->currentTarget;
    data->currentTarget = node;

    for (const Listener &listener : listeners) {
        if (listener.capture != capturePhase)
            continue;

        // A once-listener is retired before it runs, so that a handler which
        // re-dispatches the same event does not run twice.
        if (listener.once && node)
            removeSpecificListener(context, node, data->type, listener.callback, capturePhase);

        invokeListener(context, listener, event);

        if (data->immediatePropagationStopped)
            break;
    }

    for (Listener &listener : listeners)
        releaseListener(context, listener);

    data->currentTarget = previousTarget;
    return !data->propagationStopped;
}

/// The propagation path: the target, then its ancestors up to the root.
QList<dom::Node *> propagationPath(dom::Node *target)
{
    QList<dom::Node *> path;
    for (dom::Node *node = target; node; node = node->parent())
        path.append(node);
    return path;
}

/// True for the events a page registers on window rather than on a node. Only
/// these are routed there, so an event dispatched on an element is not also
/// delivered to every window listener.
bool eventTargetsWindow(const QString &type)
{
    // The lifecycle events a page waits for, plus the ones the host would fire
    // at the window itself. Everything else goes to nodes alone.
    static const QSet<QString> kWindowEvents = {
        QStringLiteral("load"),         QStringLiteral("DOMContentLoaded"),
        QStringLiteral("beforeunload"), QStringLiteral("unload"),
        QStringLiteral("resize"),       QStringLiteral("scroll"),
        QStringLiteral("hashchange"),   QStringLiteral("popstate"),
        QStringLiteral("pageshow"),     QStringLiteral("pagehide"),
    };
    return kWindowEvents.contains(type);
}

/// Runs the window's listeners for one event, in one phase.
bool runWindowListeners(JSContext *context, JSValueConst event, bool capturePhase)
{
    auto *data = eventDataOf(event);
    if (!data)
        return true;

    const auto contextEntry = windowListeners().find(context);
    if (contextEntry == windowListeners().end())
        return true;

    const auto typeEntry = contextEntry->find(data->type);
    if (typeEntry == contextEntry->end())
        return true;

    const QList<Listener> listeners = *typeEntry;

    // A once-listener on the window has to be retired from the window's own
    // table, which is keyed differently from the node one.
    QList<Listener> retained;
    retained.reserve(listeners.size());
    for (const Listener &listener : listeners) {
        if (!listener.once || listener.capture != capturePhase) {
            retained.append(listener);
            continue;
        }
        releaseListener(context, const_cast<Listener &>(listener));
    }

    if (retained.size() != listeners.size()) {
        auto &entry = windowListeners()[context][data->type];
        entry = retained;
    }

    return runListenersFrom(context, listeners, nullptr, event, capturePhase);
}

/// Delivers `event` to `target`. Shared by the script-facing dispatchEvent and
/// by the events the browser fires itself.
bool dispatchEventTo(JSContext *context, dom::Node *target, JSValueConst event)
{
    auto *data = eventDataOf(event);
    if (!data) {
        detail::throwDomError(context, QStringLiteral("TypeError"),
                              QStringLiteral("dispatchEvent needs an event"));
        return false;
    }

    const QList<dom::Node *> path = propagationPath(target);

    data->target = target;
    data->currentTarget = nullptr;

    // Capture, from the root down to the target's parent. The target's own
    // capture listeners run in the at-target phase below, so index 0 is skipped.
    bool stopped = false;
    for (int i = static_cast<int>(path.size()) - 1; i >= 1; --i) {
        data->eventPhase = 1;
        if (!runListenersOn(context, path.at(i), event, true)) {
            stopped = true;
            break;
        }
    }

    // At target: capture listeners first, then bubble listeners, both on the
    // target itself, which is what the standard specifies.
    if (!stopped) {
        data->eventPhase = 2;
        stopped = !runListenersOn(context, target, event, true);
        if (!data->immediatePropagationStopped && !stopped)
            stopped = !runListenersOn(context, target, event, false);
    }

    // Bubble, back up the path, and only for an event that bubbles.
    if (!stopped && data->bubbles) {
        for (int i = 1; i < path.size(); ++i) {
            data->eventPhase = 3;
            if (!runListenersOn(context, path.at(i), event, false))
                break;
        }
    }

    // The window is the last stop: a page registers its DOMContentLoaded and
    // load listeners there, and the browser fires those with the document as the
    // target, so the window's listeners run at the end of the walk.
    if (!stopped && eventTargetsWindow(data->type)) {
        data->eventPhase = 3;
        runWindowListeners(context, event, false);
    }

    data->eventPhase = 0;
    data->currentTarget = nullptr;
    return !data->defaultPrevented;
}

JSValue makeEventObject(JSContext *context, const QString &type, bool bubbles, bool cancelable)
{
    const detail::State *state = detail::stateFor(context);
    if (!state || !state->classes.valid)
        return JS_EXCEPTION;

    JSValue object = JS_NewObjectClass(context, state->classes.event);
    if (JS_IsException(object))
        return object;

    JS_SetPrototype(context, object, state->eventPrototype);

    auto *data = new EventData;
    data->type = type;
    data->bubbles = bubbles;
    data->cancelable = cancelable;

    if (JS_SetOpaque(object, data) < 0) {
        delete data;
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }

    return object;
}

// ---------------------------------------------------------------- accessors

JSValue eventGetType(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (EventData *data = eventDataOf(thisValue))
        return detail::newString(context, data->type);
    return JS_UNDEFINED;
}

JSValue eventGetTarget(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    EventData *data = eventDataOf(thisValue);
    return data ? detail::wrapNode(context, data->target) : JS_NULL;
}

JSValue eventGetCurrentTarget(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    // While a listener runs this is the node the walk has reached; afterwards it
    // is null, as the standard requires.
    EventData *data = eventDataOf(thisValue);
    return data ? detail::wrapNode(context, data->currentTarget) : JS_NULL;
}

JSValue eventGetEventPhase(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    EventData *data = eventDataOf(thisValue);
    return JS_NewInt32(context, data ? data->eventPhase : 0);
}

JSValue eventGetBubbles(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    EventData *data = eventDataOf(thisValue);
    return JS_NewBool(context, data && data->bubbles);
}

JSValue eventGetCancelable(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    EventData *data = eventDataOf(thisValue);
    return JS_NewBool(context, data && data->cancelable);
}

JSValue eventGetDefaultPrevented(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    EventData *data = eventDataOf(thisValue);
    return JS_NewBool(context, data && data->defaultPrevented);
}

JSValue eventGetDetail(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    // The value JavaScript stored, so it lives and dies with the event object.
    JSValue detail = JS_GetPropertyStr(context, thisValue, "__detail");
    if (JS_IsUndefined(detail)) {
        JS_FreeValue(context, detail);
        return JS_NULL;
    }
    return detail;
}

JSValue eventPreventDefault(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    Q_UNUSED(context);
    // A non-cancelable event ignores preventDefault, so a page cannot cancel
    // something the browser owns.
    if (EventData *data = eventDataOf(thisValue); data && data->cancelable)
        data->defaultPrevented = true;
    return JS_UNDEFINED;
}

JSValue eventStopPropagation(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    Q_UNUSED(context);
    if (EventData *data = eventDataOf(thisValue))
        data->propagationStopped = true;
    return JS_UNDEFINED;
}

JSValue eventStopImmediatePropagation(JSContext *context, JSValueConst thisValue, int,
                                      JSValueConst *)
{
    Q_UNUSED(context);
    if (EventData *data = eventDataOf(thisValue)) {
        data->propagationStopped = true;
        // This also skips the listeners left on the node being dispatched, which
        // is the difference between the two stop methods.
        data->immediatePropagationStopped = true;
    }
    return JS_UNDEFINED;
}

JSValue eventComposedPath(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    // The path is not retained after dispatch, so this reports just the target.
    // Pages use it to inspect shadow trees, which this engine does not have.
    EventData *data = eventDataOf(thisValue);
    JSValue array = JS_NewArray(context);
    if (data && data->target)
        JS_SetPropertyUint32(context, array, 0, detail::wrapNode(context, data->target));
    return array;
}

} // namespace

// -------------------------------------------------------------------- Events

JSClassFinalizer *Events::finalizer()
{
    return finalizeEvent;
}

void Events::installMembers(JSContext *context, JSValue eventPrototype)
{
    // Only the members every event has. The interface-specific ones, such as a
    // MouseEvent's coordinates, are plain properties on the instance: they are
    // data rather than behaviour, so copying them is cheaper than a second
    // prototype chain.
    detail::defineGetter(context, eventPrototype, "type", eventGetType);
    detail::defineGetter(context, eventPrototype, "target", eventGetTarget);
    detail::defineGetter(context, eventPrototype, "currentTarget", eventGetCurrentTarget);
    detail::defineGetter(context, eventPrototype, "eventPhase", eventGetEventPhase);
    detail::defineGetter(context, eventPrototype, "bubbles", eventGetBubbles);
    detail::defineGetter(context, eventPrototype, "cancelable", eventGetCancelable);
    detail::defineGetter(context, eventPrototype, "defaultPrevented", eventGetDefaultPrevented);
    detail::defineGetter(context, eventPrototype, "detail", eventGetDetail);

    // A read-only property, because this engine never runs a real event loop to
    // pass a timeout to.
    detail::defineInt(context, eventPrototype, "NONE", 0);
    detail::defineInt(context, eventPrototype, "CAPTURING_PHASE", 1);
    detail::defineInt(context, eventPrototype, "AT_TARGET", 2);
    detail::defineInt(context, eventPrototype, "BUBBLING_PHASE", 3);

    detail::defineMethod(context, eventPrototype, "preventDefault", eventPreventDefault, 0);
    detail::defineMethod(context, eventPrototype, "stopPropagation", eventStopPropagation, 0);
    detail::defineMethod(context, eventPrototype, "stopImmediatePropagation",
                         eventStopImmediatePropagation, 0);
    detail::defineMethod(context, eventPrototype, "composedPath", eventComposedPath, 0);
}

JSValue Events::addListener(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Node *node = detail::thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    if (argc < 2) {
        return detail::throwDomError(
            context, QStringLiteral("TypeError"),
            QStringLiteral("addEventListener needs a type and a listener"));
    }

    Listener listener;
    if (!listenerFromArgument(context, argv[1], &listener))
        return JS_UNDEFINED; // an unusable listener is ignored rather than an error

    readListenerOptions(context, argc > 2 ? argv[2] : JS_UNDEFINED, &listener.capture,
                        &listener.once);

    // The type is used exactly as written. Event types are case-sensitive in the
    // DOM, so lower-casing here would stop a listener for `DOMContentLoaded`
    // from ever matching the event the browser dispatches.
    const QString type = detail::stringValue(context, argv[0]);
    (void)registries(); // ensure the table exists before the first insert
    registries()[context][node][type].append(listener);
    return JS_UNDEFINED;
}

JSValue Events::removeListener(JSContext *context, JSValueConst thisValue, int argc,
                               JSValueConst *argv)
{
    dom::Node *node = detail::thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    if (argc < 2)
        return JS_UNDEFINED;

    const QString type = detail::stringValue(context, argv[0]);
    bool capture = false;
    bool once = false;
    readListenerOptions(context, argc > 2 ? argv[2] : JS_UNDEFINED, &capture, &once);

    removeSpecificListener(context, node, type, argv[1], capture);
    return JS_UNDEFINED;
}

JSValue Events::dispatchFromScript(JSContext *context, JSValueConst thisValue, int argc,
                                   JSValueConst *argv)
{
    dom::Node *node = detail::thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    if (argc < 1) {
        return detail::throwDomError(context, QStringLiteral("TypeError"),
                                     QStringLiteral("dispatchEvent needs an event"));
    }

    if (!eventDataOf(argv[0])) {
        return detail::throwDomError(context, QStringLiteral("TypeError"),
                                     QStringLiteral("the argument is not an event"));
    }

    // False when a cancelable event was cancelled, which is how script tests
    // whether a handler called preventDefault.
    return JS_NewBool(context, dispatchEventTo(context, node, argv[0]));
}

bool Events::dispatch(JSContext *context, dom::Node *target, const QString &type, bool bubbles,
                      bool cancelable)
{
    JSValue event = makeEventObject(context, type, bubbles, cancelable);
    if (JS_IsException(event) || JS_IsUndefined(event))
        return true;

    const bool notCancelled = dispatchEventTo(context, target, event);
    JS_FreeValue(context, event);
    return notCancelled;
}

JSValue Events::makeEvent(JSContext *context, const QString &type, bool bubbles, bool cancelable)
{
    return makeEventObject(context, type, bubbles, cancelable);
}

JSValue Events::makeEventWithDetail(JSContext *context, const QString &type, JSValueConst detail,
                                    bool bubbles, bool cancelable)
{
    JSValue event = makeEventObject(context, type, bubbles, cancelable);
    if (JS_IsException(event))
        return event;

    JS_DefinePropertyValueStr(context, event, "__detail", JS_DupValue(context, detail),
                              JS_PROP_C_W_E);
    return event;
}

JSValue Events::addWindowListener(JSContext *context, JSValueConst thisValue, int argc,
                                   JSValueConst *argv)
{
    // The receiver is the global object, which is what makes these the window's
    // listeners rather than a node's.
    Q_UNUSED(thisValue);
    if (argc < 2) {
        return detail::throwDomError(
            context, QStringLiteral("TypeError"),
            QStringLiteral("addEventListener needs a type and a listener"));
    }

    Listener listener;
    if (!listenerFromArgument(context, argv[1], &listener))
        return JS_UNDEFINED;

    readListenerOptions(context, argc > 2 ? argv[2] : JS_UNDEFINED, &listener.capture,
                        &listener.once);

    const QString type = detail::stringValue(context, argv[0]);
    (void)windowListeners();
    windowListeners()[context][type].append(listener);
    return JS_UNDEFINED;
}

JSValue Events::removeWindowListener(JSContext *context, JSValueConst thisValue, int argc,
                                      JSValueConst *argv)
{
    Q_UNUSED(thisValue);
    if (argc < 2)
        return JS_UNDEFINED;

    const QString type = detail::stringValue(context, argv[0]);
    bool capture = false;
    bool once = false;
    readListenerOptions(context, argc > 2 ? argv[2] : JS_UNDEFINED, &capture, &once);

    const auto contextEntry = windowListeners().find(context);
    if (contextEntry == windowListeners().end())
        return JS_UNDEFINED;

    const auto typeEntry = contextEntry->find(type);
    if (typeEntry == contextEntry->end())
        return JS_UNDEFINED;

    for (int i = static_cast<int>(typeEntry->size()) - 1; i >= 0; --i) {
        Listener &listener = (*typeEntry)[i];
        if (listener.capture != capture || !sameCallback(listener.callback, argv[1]))
            continue;
        releaseListener(context, listener);
        typeEntry->removeAt(i);
        break;
    }

    if (typeEntry->isEmpty())
        contextEntry->remove(type);

    return JS_UNDEFINED;
}

void Events::dispatchLifecycle(JSContext *context, dom::Node *document, const QString &type,
                               bool bubbles, bool cancelable)
{
    // The event is dispatched on the document, and the walk reaches the window's
    // listeners at the end, which is where a page registers DOMContentLoaded and
    // load.
    dispatch(context, document, type, bubbles, cancelable);
}

void Events::clearContext(JSContext *context)
{
    const auto it = registries().find(context);
    if (it == registries().end())
        return;

    // Every callback is a value of this context, so they must be released before
    // the context itself is freed.
    for (auto &listenerMap : it.value()) {
        for (auto &listeners : listenerMap) {
            for (Listener &listener : listeners)
                releaseListener(context, listener);
        }
    }
    registries().remove(context);

    // The window's listeners hold values of this context too.
    const auto windowEntry = windowListeners().find(context);
    if (windowEntry == windowListeners().end())
        return;

    for (auto &listeners : windowEntry.value()) {
        for (Listener &listener : listeners)
            releaseListener(context, listener);
    }
    windowListeners().remove(context);
}

} // namespace oqb::javascript

// The Event prototype members and the constructors pages call live in
// EventsMembers.cpp, so that this file keeps to the listener registry and the
// dispatch walk.

#endif // OPENQBROWSER_SCRIPTING
