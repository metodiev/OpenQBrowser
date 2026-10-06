#include "javascript/BindingsInternal.h"

#include "javascript/Events.h"

#include <algorithm>
#include "javascript/TimerQueue.h"

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript {

namespace detail {

namespace {
/// The state table. QuickJS has nowhere to hang per-context data, so it is keyed
/// by the context pointer. One engine owns one context and the entry lives as
/// long as the runtime, so the table stays tiny.
QHash<JSContext *, State *> &stateTable()
{
    static QHash<JSContext *, State *> table;
    return table;
}

/// Releases a wrapper object. The wrapper holds no ownership: a detached node
/// belongs to the graveyard, so there is nothing to delete here.
void finalizeWrapper(JSRuntime *runtime, JSValueConst value)
{
    Q_UNUSED(runtime);
    delete static_cast<Wrapper *>(opaqueOf(value));
}

JSClassID registerClass(JSRuntime *runtime, const char *name, JSClassFinalizer *finalizer)
{
    JSClassID id = 0;
    JS_NewClassID(runtime, &id);

    JSClassDef definition = {};
    definition.class_name = name;
    definition.finalizer = finalizer;
    JS_NewClass(runtime, id, &definition);
    return id;
}

/// One class per name, per runtime. Auxiliary objects are created often - every
/// `el.classList` access builds one - so registering a fresh class each time
/// would leak ids without bound.
QHash<QString, JSClassID> &auxiliaryClasses(JSRuntime *runtime)
{
    static QHash<JSRuntime *, QHash<QString, JSClassID>> tables;
    return tables[runtime];
}
} // namespace

State *stateFor(JSContext *context)
{
    const auto it = stateTable().find(context);
    return it == stateTable().end() ? nullptr : it.value();
}

void setState(JSContext *context, State state)
{
    auto *stored = new State(std::move(state));
    stateTable().insert(context, stored);
}

void destroyState(JSContext *context)
{
    const auto it = stateTable().find(context);
    if (it == stateTable().end())
        return;

    State *state = it.value();

    // The listeners hold values, so they are released before the prototypes and
    // the wrapper cache. All of them belong to this context.
    Events::clearContext(context);

    for (auto &wrapper : state->wrappers)
        JS_FreeValue(context, wrapper);

    JS_FreeValue(context, state->nodePrototype);
    JS_FreeValue(context, state->elementPrototype);
    JS_FreeValue(context, state->documentPrototype);
    JS_FreeValue(context, state->eventPrototype);

    // Everything script detached dies with the context. This is the one place
    // those nodes are freed, which is what makes the graveyard safe.
    state->graveyard.clear();

    stateTable().remove(context);
    delete state;
}

void detachFromTree(JSContext *context, dom::Node *node)
{
    State *state = stateFor(context);
    if (!state || !node)
        return;

    // Only a node that is currently in a tree needs moving, and only the tree it
    // belongs to may hand it over. A node already in the graveyard is left
    // alone, which is what makes detaching twice harmless.
    dom::Node *parent = node->parent();
    if (!parent)
        return;

    if (std::unique_ptr<dom::Node> removed = parent->removeChild(node))
        state->graveyard.emplace_back(node, std::move(removed));
}

std::unique_ptr<dom::Node> adoptFromGraveyard(JSContext *context, dom::Node *node)
{
    State *state = stateFor(context);
    if (!state || !node)
        return nullptr;

    for (auto it = state->graveyard.begin(); it != state->graveyard.end(); ++it) {
        if (it->first != node)
            continue;
        std::unique_ptr<dom::Node> owned = std::move(it->second);
        state->graveyard.erase(it);
        return owned;
    }
    return nullptr;
}

void ownNewNode(JSContext *context, dom::Node *node)
{
    State *state = stateFor(context);
    if (!state || !node)
        return;

    // A node that already has a parent belongs to the tree, so nothing to do.
    if (node->parent())
        return;

    const auto existing = std::find_if(state->graveyard.begin(), state->graveyard.end(),
                                       [node](const auto &entry) { return entry.first == node; });
    if (existing != state->graveyard.end())
        return;

    state->graveyard.emplace_back(node, std::unique_ptr<dom::Node>(node));
}

void clearChildrenKeepingScript(JSContext *context, dom::Node *parent)
{
    if (!parent)
        return;

    // Children are taken one at a time so that each keeps its identity: script
    // holding a reference to one of them must still find it usable afterwards.
    while (dom::Node *child = parent->firstChild())
        detachFromTree(context, child);
}

void reportMessage(JSContext *context, ConsoleMessage::Level level, const QString &text)
{
    State *state = stateFor(context);
    if (!state || !state->messages)
        return;

    ConsoleMessage message;
    message.level = level;
    message.text = text;
    state->messages->append(message);
}

void *opaqueOf(JSValueConst value)
{
    if (!JS_IsObject(value))
        return nullptr;

    JSClassID classId = 0;
    return JS_GetAnyOpaque(value, &classId);
}

Wrapper *wrapperOf(JSValueConst value)
{
    return static_cast<Wrapper *>(opaqueOf(value));
}

JSValue wrapNode(JSContext *context, dom::Node *node)
{
    if (!node)
        return JS_NULL;

    State *state = stateFor(context);
    if (!state || !state->classes.valid)
        return JS_NULL;

    // One wrapper per node: `el === document.body` has to be true, and a
    // listener's event.target has to be the same object the page looked up.
    if (const auto existing = state->wrappers.constFind(node); existing != state->wrappers.end())
        return JS_DupValue(context, existing.value());

    JSClassID classId = state->classes.node;
    JSValue prototype = state->nodePrototype;

    if (node->isElement()) {
        classId = state->classes.element;
        prototype = state->elementPrototype;
    } else if (node->isDocument()) {
        classId = state->classes.document;
        prototype = state->documentPrototype;
    }

    JSValue object = JS_NewObjectClass(context, classId);
    if (JS_IsException(object))
        return object;

    JS_SetPrototype(context, object, prototype);

    auto *wrapper = new Wrapper;
    wrapper->node = node;

    if (JS_SetOpaque(object, wrapper) < 0) {
        delete wrapper;
        JS_FreeValue(context, object);
        return throwDomError(context, QStringLiteral("Error"),
                             QStringLiteral("could not attach a DOM wrapper"));
    }

    // The cache holds a reference of its own, so the wrapper survives even when
    // script drops every reference it has to it.
    state->wrappers.insert(node, JS_DupValue(context, object));
    return object;
}

JSClassID auxiliaryClassId(JSContext *context, const char *name, JSClassFinalizer *finalizer)
{
    JSRuntime *runtime = JS_GetRuntime(context);

    const QString key = QString::fromLatin1(name);
    QHash<QString, JSClassID> &table = auxiliaryClasses(runtime);

    const auto existing = table.constFind(key);
    if (existing != table.constEnd())
        return existing.value();

    const JSClassID id = registerClass(runtime, name, finalizer);
    table.insert(key, id);
    return id;
}

dom::Document *documentOf(JSContext *context)
{
    State *state = stateFor(context);
    return state ? state->document : nullptr;
}

QString nodeNameOf(const dom::Node *node)
{
    if (node->isElement())
        return static_cast<const dom::Element *>(node)->tagName().toUpper();
    return node->nodeName().toLower();
}

int nodeTypeOf(const dom::Node *node)
{
    return static_cast<int>(node->type());
}

JSValue newString(JSContext *context, const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    return JS_NewStringLen(context, utf8.constData(), static_cast<size_t>(utf8.size()));
}

QString stringValue(JSContext *context, JSValueConst value)
{
    if (const char *text = JS_ToCString(context, value)) {
        const QString result = QString::fromUtf8(text);
        JS_FreeCString(context, text);
        return result;
    }
    return {};
}

int intValue(JSContext *context, JSValueConst value)
{
    int32_t result = 0;
    JS_ToInt32(context, &result, value);
    return static_cast<int>(result);
}

bool boolValue(JSContext *context, JSValueConst value)
{
    return JS_ToBool(context, value) > 0;
}

void defineGetter(JSContext *context, JSValueConst object, const char *name, JSCFunction *getter)
{
    // Every DOM member is an accessor rather than a stored property, because
    // reading it must consult the live tree: `el.textContent` has to reflect the
    // current children, not a snapshot taken when the wrapper was created.
    JSAtom atom = JS_NewAtom(context, name);
    JSValue function = JS_NewCFunction(context, getter, name, 0);
    JS_DefinePropertyGetSet(context, object, atom, function, JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_HAS_GET);
    JS_FreeAtom(context, atom);
}

void defineAccessor(JSContext *context, JSValueConst object, const char *name,
                    JSCFunction *getter, JSCFunction *setter)
{
    JSAtom atom = JS_NewAtom(context, name);
    JSValue getterFunction = JS_NewCFunction(context, getter, name, 0);
    JSValue setterFunction = setter ? JS_NewCFunction(context, setter, name, 1) : JS_UNDEFINED;

    int flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE | JS_PROP_HAS_GET;
    if (setter)
        flags |= JS_PROP_HAS_SET;

    JS_DefinePropertyGetSet(context, object, atom, getterFunction, setterFunction, flags);
    JS_FreeAtom(context, atom);
}

void defineMethod(JSContext *context, JSValueConst object, const char *name, JSCFunction *function,
                  int length)
{
    JSValue fn = JS_NewCFunction(context, function, name, length);
    JS_DefinePropertyValueStr(context, object, name, fn, JS_PROP_C_W_E);
}

void defineMagicMethod(JSContext *context, JSValueConst object, const char *name,
                       JSCFunctionMagic *function, int length, int magic)
{
    JSValue fn
        = JS_NewCFunctionMagic(context, function, name, length, JS_CFUNC_generic_magic, magic);
    JS_DefinePropertyValueStr(context, object, name, fn, JS_PROP_C_W_E);
}

void defineInt(JSContext *context, JSValueConst object, const char *name, int value)
{
    JS_DefinePropertyValueStr(context, object, name, JS_NewInt32(context, value), JS_PROP_C_W_E);
}

void defineString(JSContext *context, JSValueConst object, const char *name, const QString &value)
{
    JS_DefinePropertyValueStr(context, object, name, newString(context, value), JS_PROP_C_W_E);
}

void touchDocument(JSContext *context)
{
    if (State *state = stateFor(context))
        state->documentTouched = true;
}

JSValue throwDomError(JSContext *context, const QString &name, const QString &message)
{
    // DOM code fails with an exception carrying a name script can branch on, so
    // the error is shaped like the ones a browser throws rather than being a bare
    // string.
    JSValue error = JS_NewError(context);
    JS_DefinePropertyValueStr(context, error, "name", newString(context, name), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, error, "message", newString(context, message),
                              JS_PROP_C_W_E);
    return JS_Throw(context, error);
}

dom::Node *thisNode(JSContext *context, JSValueConst thisValue)
{
    Wrapper *wrapper = wrapperOf(thisValue);
    if (!wrapper || !wrapper->node) {
        throwDomError(context, QStringLiteral("TypeError"), QStringLiteral("this is not a node"));
        return nullptr;
    }
    return wrapper->node;
}

dom::Element *thisElement(JSContext *context, JSValueConst thisValue)
{
    Wrapper *wrapper = wrapperOf(thisValue);
    if (!wrapper || !wrapper->node || !wrapper->node->isElement()) {
        throwDomError(context, QStringLiteral("TypeError"),
                      QStringLiteral("this is not an element"));
        return nullptr;
    }
    return static_cast<dom::Element *>(wrapper->node);
}

dom::Document *thisDocument(JSContext *context, JSValueConst thisValue)
{
    if (dom::Node *node = thisNode(context, thisValue); node && node->isDocument())
        return static_cast<dom::Document *>(node);
    return Bindings::documentOf(context);
}

} // namespace detail

// ------------------------------------------------------------- Bindings

JSValue Bindings::install(JSContext *context, ClassIds &classes, dom::Document *document,
                          TimerQueue *timers, QList<ConsoleMessage> *messages)
{
    JSRuntime *runtime = JS_GetRuntime(context);

    classes.node = detail::registerClass(runtime, "Node", detail::finalizeWrapper);
    classes.element = detail::registerClass(runtime, "Element", detail::finalizeWrapper);
    classes.document = detail::registerClass(runtime, "Document", detail::finalizeWrapper);
    classes.event = detail::registerClass(runtime, "Event", Events::finalizer());
    classes.valid = true;

    detail::State state;
    state.classes = classes;
    state.document = document;
    state.timers = timers;
    state.messages = messages;
    detail::setState(context, std::move(state));

    detail::State *installed = detail::stateFor(context);

    // The prototypes are built before anything uses them. Element's and
    // Document's inherit from Node's, so neither repeats the node members.
    installed->nodePrototype = JS_NewObject(context);
    installed->elementPrototype = JS_NewObject(context);
    installed->documentPrototype = JS_NewObject(context);
    installed->eventPrototype = JS_NewObject(context);

    JS_SetPrototype(context, installed->elementPrototype, installed->nodePrototype);
    JS_SetPrototype(context, installed->documentPrototype, installed->nodePrototype);

    detail::installNodeMembers(context, installed->nodePrototype);
    detail::installElementMembers(context, installed->elementPrototype);
    detail::installDocumentMembers(context, installed->documentPrototype);
    Events::installMembers(context, installed->eventPrototype);

    JSValue global = JS_GetGlobalObject(context);

    detail::installGlobalMembers(context, global, document);
    detail::installTimerMembers(context, global);
    detail::installConsole(context, global);
    Events::installConstructors(context, global);

    // The DOM constructors. They are not callable in a browser either, so a bare
    // function with the right prototype is enough for `instanceof` and for
    // `String(Element)`.
    const struct
    {
        const char *name;
        const char *kind;
    } constructors[] = {
        {"Node", "node"},
        {"Element", "element"},
        {"HTMLElement", "element"},
        {"HTMLDocument", "document"},
    };

    for (const auto &entry : constructors) {
        JSValue constructor = JS_NewCFunction(context, nullptr, entry.name, 0);
        JS_DefinePropertyValueStr(
            context, constructor, "prototype",
            JS_DupValue(context, prototype(context, QString::fromLatin1(entry.kind))),
            JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(context, global, entry.name, constructor, JS_PROP_C_W_E);
    }

    // window, self and top all refer to the global object, as in a browser.
    JS_DefinePropertyValueStr(context, global, "window", JS_DupValue(context, global),
                              JS_PROP_C_W_E);

    // The events a page registers on window go to a separate table, because the
    // window is the global object rather than a node. The browser fires
    // DOMContentLoaded and load at the document and the walk reaches these.
    detail::defineMethod(context, global, "addEventListener", Events::addWindowListener, 2);
    detail::defineMethod(context, global, "removeEventListener", Events::removeWindowListener, 2);
    JS_DefinePropertyValueStr(context, global, "self", JS_DupValue(context, global),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(context, global, "top", JS_DupValue(context, global),
                              JS_PROP_C_W_E);

    // The document is installed last, once the wraps it needs are all in place.
    setDocument(context, document);

    return global;
}

void Bindings::setDocument(JSContext *context, dom::Document *document)
{
    detail::State *state = detail::stateFor(context);
    if (!state)
        return;

    state->document = document;

    JSValue global = JS_GetGlobalObject(context);
    JS_DefinePropertyValueStr(context, global, "document", detail::wrapNode(context, document),
                              JS_PROP_C_W_E);
    JS_FreeValue(context, global);
}

void Bindings::destroy(JSContext *context)
{
    detail::destroyState(context);
}

dom::Document *Bindings::documentOf(JSContext *context)
{
    detail::State *state = detail::stateFor(context);
    return state ? state->document : nullptr;
}

const Bindings::ClassIds &Bindings::classIdsOf(JSContext *context)
{
    static const ClassIds kNone;
    detail::State *state = detail::stateFor(context);
    return state ? state->classes : kNone;
}

void Bindings::touchDocument(JSContext *context)
{
    detail::touchDocument(context);
}

bool Bindings::takeDocumentTouched(JSContext *context)
{
    detail::State *state = detail::stateFor(context);
    if (!state)
        return false;
    const bool touched = state->documentTouched;
    state->documentTouched = false;
    return touched;
}

JSValue Bindings::newString(JSContext *context, const QString &text)
{
    return detail::newString(context, text);
}

QString Bindings::stringValue(JSContext *context, JSValueConst value)
{
    return detail::stringValue(context, value);
}

int Bindings::intProperty(JSContext *context, JSValueConst object, const char *name)
{
    JSValue value = JS_GetPropertyStr(context, object, name);
    const int result = detail::intValue(context, value);
    JS_FreeValue(context, value);
    return result;
}

JSValue Bindings::prototype(JSContext *context, const QString &kind)
{
    detail::State *state = detail::stateFor(context);
    if (!state)
        return JS_UNDEFINED;

    if (kind == QLatin1String("node"))
        return state->nodePrototype;
    if (kind == QLatin1String("element"))
        return state->elementPrototype;
    if (kind == QLatin1String("document"))
        return state->documentPrototype;
    if (kind == QLatin1String("event"))
        return state->eventPrototype;
    return JS_UNDEFINED;
}

Bindings::WrapperState *Bindings::wrapperState(JSValueConst value)
{
    // Wrapper and WrapperState are the same layout by design, so the opaque block
    // a wrapper was allocated with can be read through either name.
    return reinterpret_cast<WrapperState *>(detail::wrapperOf(value));
}

JSValue Bindings::wrapNode(JSContext *context, dom::Node *node, bool)
{
    // The second argument is kept for call sites that used to distinguish
    // borrowed from owned nodes. Ownership now lives in the graveyard, so the
    // wrapper is the same either way.
    return detail::wrapNode(context, node);
}

dom::Node *Bindings::nodeOf(JSValueConst value)
{
    detail::Wrapper *wrapper = detail::wrapperOf(value);
    return wrapper ? wrapper->node : nullptr;
}

void Bindings::detachNode(JSContext *context, dom::Node *node)
{
    detail::detachFromTree(context, node);
}

std::unique_ptr<dom::Node> Bindings::adoptNode(JSContext *context, dom::Node *node)
{
    return detail::adoptFromGraveyard(context, node);
}

void Bindings::ownNewNode(JSContext *context, dom::Node *node)
{
    detail::ownNewNode(context, node);
}

JSValue Bindings::throwDomError(JSContext *context, const QString &name, const QString &message)
{
    return detail::throwDomError(context, name, message);
}

} // namespace oqb::javascript

#endif // OPENQBROWSER_SCRIPTING
