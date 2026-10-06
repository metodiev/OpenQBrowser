#include "javascript/Events.h"

#include "javascript/Bindings.h"
#include "javascript/BindingsInternal.h"

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript {
namespace {

/// The interfaces a constructor can be called as. They differ only in the extra
/// members they carry, so one function serves all of them and the magic number
/// says which.
enum EventInterface {
    InterfaceEvent = 0,
    InterfaceCustomEvent,
    InterfaceMouseEvent,
    InterfaceKeyboardEvent,
    InterfaceFocusEvent,
    InterfaceInputEvent,
};

/// Reads `{ bubbles, cancelable }` from an event's init dictionary. A missing
/// dictionary is legal: `new Event("x")` gives a non-bubbling, non-cancelable
/// event, which is what the standard says.
void readInit(JSContext *context, JSValueConst init, bool *bubbles, bool *cancelable)
{
    *bubbles = false;
    *cancelable = false;

    if (!JS_IsObject(init) || JS_IsNull(init))
        return;

    JSValue bubblesValue = JS_GetPropertyStr(context, init, "bubbles");
    if (!JS_IsUndefined(bubblesValue))
        *bubbles = JS_ToBool(context, bubblesValue) > 0;
    JS_FreeValue(context, bubblesValue);

    JSValue cancelableValue = JS_GetPropertyStr(context, init, "cancelable");
    if (!JS_IsUndefined(cancelableValue))
        *cancelable = JS_ToBool(context, cancelableValue) > 0;
    JS_FreeValue(context, cancelableValue);
}

JSValue initProperty(JSContext *context, JSValueConst init, const char *name)
{
    if (!JS_IsObject(init) || JS_IsNull(init))
        return JS_UNDEFINED;
    return JS_GetPropertyStr(context, init, name);
}

/// Copies the coordinates and modifiers a MouseEvent carries from the init
/// dictionary onto the event as plain properties. They are data, not behaviour,
/// so plain properties are enough and cheaper than a second prototype chain.
void copyMouseFields(JSContext *context, JSValueConst init, JSValue event)
{
    static const char *const kFields[] = {"clientX", "clientY", "screenX", "screenY", "pageX",
                                          "pageY",   "button",  "buttons", "altKey", "ctrlKey",
                                          "metaKey", "shiftKey"};

    for (const char *field : kFields) {
        const bool boolean = QString::fromLatin1(field).endsWith(QLatin1String("Key"));
        JSValue value = initProperty(context, init, field);

        if (JS_IsUndefined(value)) {
            JS_FreeValue(context, value);
            // A missing coordinate reads as 0 and a missing modifier as false, so
            // a handler doing arithmetic on them never sees undefined.
            value = boolean ? JS_NewBool(context, false) : JS_NewInt32(context, 0);
        }

        JS_DefinePropertyValueStr(context, event, field, value, JS_PROP_C_W_E);
    }
}

void copyKeyboardFields(JSContext *context, JSValueConst init, JSValue event)
{
    static const char *const kStringFields[] = {"key", "code"};
    static const char *const kBoolFields[]
        = {"altKey", "ctrlKey", "metaKey", "shiftKey", "repeat", "isComposing"};

    for (const char *field : kStringFields) {
        JSValue value = initProperty(context, init, field);
        if (JS_IsUndefined(value)) {
            JS_FreeValue(context, value);
            value = detail::newString(context, QString());
        }
        JS_DefinePropertyValueStr(context, event, field, value, JS_PROP_C_W_E);
    }

    for (const char *field : kBoolFields) {
        JSValue value = initProperty(context, init, field);
        const bool on = !JS_IsUndefined(value) && JS_ToBool(context, value) > 0;
        JS_FreeValue(context, value);
        JS_DefinePropertyValueStr(context, event, field, JS_NewBool(context, on), JS_PROP_C_W_E);
    }
}

/// The constructor behind `new Event(type, init)` and its relatives.
JSValue eventConstructor(JSContext *context, JSValueConst, int argc, JSValueConst *argv, int magic)
{
    if (argc < 1) {
        return detail::throwDomError(context, QStringLiteral("TypeError"),
                                     QStringLiteral("the event type is required"));
    }

    const QString type = detail::stringValue(context, argv[0]);
    JSValueConst init = argc > 1 ? argv[1] : JS_UNDEFINED;

    bool bubbles = false;
    bool cancelable = false;
    readInit(context, init, &bubbles, &cancelable);

    JSValue event;
    if (magic == InterfaceCustomEvent) {
        // `detail` is the one member that distinguishes a CustomEvent, and it
        // defaults to null rather than undefined.
        JSValue detail = initProperty(context, init, "detail");
        event = Events::makeEventWithDetail(
            context, type, JS_IsUndefined(detail) ? JS_NULL : detail, bubbles, cancelable);
        if (!JS_IsUndefined(detail))
            JS_FreeValue(context, detail);
    } else {
        event = Events::makeEvent(context, type, bubbles, cancelable);
    }

    if (JS_IsException(event))
        return event;

    if (magic == InterfaceMouseEvent)
        copyMouseFields(context, init, event);
    else if (magic == InterfaceKeyboardEvent)
        copyKeyboardFields(context, init, event);
    else if (magic == InterfaceFocusEvent)
        // The element that lost or gained focus; this engine does not track it,
        // so it reads as null, which page code guards against anyway.
        JS_DefinePropertyValueStr(context, event, "relatedTarget", JS_NULL, JS_PROP_C_W_E);
    else if (magic == InterfaceInputEvent)
        JS_DefinePropertyValueStr(context, event, "data", JS_NULL, JS_PROP_C_W_E);

    return event;
}

/// Installs one constructor as a global. All of them share the single event
/// prototype, so `event instanceof Event` is true whichever built the event.
void installConstructor(JSContext *context, JSValue global, const char *name, int interface,
                        JSValueConst prototype)
{
    JSValue constructor = JS_NewCFunctionMagic(context, eventConstructor, name, 1,
                                               JS_CFUNC_constructor_magic, interface);

    // `Event.prototype.constructor === Event` is relied on by a fair amount of
    // page code, so the link is set both ways.
    JS_DefinePropertyValueStr(context, prototype, "constructor", JS_DupValue(context, constructor),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, constructor, "prototype", JS_DupValue(context, prototype),
                              JS_PROP_C_W_E);

    // The phase constants live on the constructor, as in a browser.
    detail::defineInt(context, constructor, "NONE", 0);
    detail::defineInt(context, constructor, "CAPTURING_PHASE", 1);
    detail::defineInt(context, constructor, "AT_TARGET", 2);
    detail::defineInt(context, constructor, "BUBBLING_PHASE", 3);

    JS_DefinePropertyValueStr(context, global, name, constructor, JS_PROP_C_W_E);
}

} // namespace

void Events::installConstructors(JSContext *context, JSValue global)
{
    const detail::State *state = detail::stateFor(context);
    if (!state)
        return;

    const JSValue prototype = state->eventPrototype;

    installConstructor(context, global, "Event", InterfaceEvent, prototype);
    installConstructor(context, global, "CustomEvent", InterfaceCustomEvent, prototype);
    installConstructor(context, global, "MouseEvent", InterfaceMouseEvent, prototype);
    installConstructor(context, global, "KeyboardEvent", InterfaceKeyboardEvent, prototype);
    installConstructor(context, global, "FocusEvent", InterfaceFocusEvent, prototype);
    installConstructor(context, global, "InputEvent", InterfaceInputEvent, prototype);
}

} // namespace oqb::javascript

#endif // OPENQBROWSER_SCRIPTING
