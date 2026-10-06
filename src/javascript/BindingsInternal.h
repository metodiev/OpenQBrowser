#pragma once

// Shared internals for the JavaScript bindings.
//
// The bindings are split across several translation units so that each stays
// readable: this header carries the state and the small helpers they all need.
// It is not part of the browser's interface and nothing outside src/javascript
// includes it.
//
// ## Node ownership
//
// The DOM owns tree nodes through std::unique_ptr, but script holds references
// that outlive a node's removal from the tree: `var el = document.getElementById
// ("x"); el.remove();` must leave `el` usable. A single rule makes that safe:
//
//   Every node that is not in the document tree is owned by the script
//   graveyard, for the lifetime of the page's context.
//
// So a node is owned by exactly one of the tree or the graveyard, never both,
// and never by a wrapper. Detaching a node moves it to the graveyard; inserting
// one takes it back out. Nothing is freed while script could still reach it,
// which trades a bounded amount of memory for the absence of use-after-free.
// The graveyard is emptied when the context is destroyed.

#include "javascript/Bindings.h"

#include "dom/Document.h"
#include "dom/Node.h"

#include "css/Selector.h"
#include "html/Parser.h"

#include <utility>
#include <vector>

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript::detail {

/// What a wrapper object holds: the node it stands for.
///
/// There is deliberately no ownership flag here. Ownership is a property of the
/// tree and the graveyard, not of a wrapper: a wrapper is only ever a view, and
/// whether the node it points at is attached or detached is answered by asking
/// the node itself. That is what lets a node be inserted into the tree after
/// script detached it without any flag having to be updated.
struct Wrapper
{
    dom::Node *node = nullptr;
};

/// One shared prototype per wrapper kind, the wrapper cache, and the graveyard.
///
/// QuickJS has no per-context user data slot, so this is looked up by context
/// pointer; one engine owns one context and the entry lives as long as the
/// runtime.
struct State
{
    Bindings::ClassIds classes;
    dom::Document *document = nullptr;
    TimerQueue *timers = nullptr;
    QList<ConsoleMessage> *messages = nullptr;

    /// One prototype object per wrapper kind. Every wrapper of that kind shares
    /// it, so a page with thousands of nodes does not pay for a method per node.
    JSValue nodePrototype = JS_UNDEFINED;
    JSValue elementPrototype = JS_UNDEFINED;
    JSValue documentPrototype = JS_UNDEFINED;
    JSValue eventPrototype = JS_UNDEFINED;

    /// One wrapper per node, so that `el === document.body` is true and a
    /// listener's `event.target` is the same object the page looked up. Entries
    /// hold a reference and are released at teardown.
    QHash<dom::Node *, JSValue> wrappers;

    /// Nodes that script detached, or that were created and never inserted.
    /// Owned here until the context is destroyed. A vector rather than a QHash
    /// because QHash cannot store a move-only value, and the lookups are rare
    /// enough that a linear scan is not worth avoiding.
    std::vector<std::pair<dom::Node *, std::unique_ptr<dom::Node>>> graveyard;

    /// Set by any binding that changes the document, and consumed by the browser
    /// to decide whether to re-style and re-lay out.
    bool documentTouched = false;
};

/// The state for a context, or nullptr when the context has no bindings.
State *stateFor(JSContext *context);

/// Registers a context's state so the later bindings can find it.
void setState(JSContext *context, State state);

/// Releases everything the state holds. Called before the runtime is freed.
void destroyState(JSContext *context);

/// Takes `node` out of the tree and hands it to the graveyard, so that any
/// wrapper script holds keeps working.
void detachFromTree(JSContext *context, dom::Node *node);

/// Takes `node` out of the graveyard, transferring ownership to the caller.
/// Returns null when the node is not there, which means it is still in the tree.
std::unique_ptr<dom::Node> adoptFromGraveyard(JSContext *context, dom::Node *node);

/// Puts a freshly created node under the graveyard's ownership, so that it has
/// an owner from the moment it exists.
void ownNewNode(JSContext *context, dom::Node *node);

/// Replaces every child of `parent` with nothing, keeping the old children
/// reachable from script.
void clearChildrenKeepingScript(JSContext *context, dom::Node *parent);

/// Reports a message to the console the page writes to.
void reportMessage(JSContext *context, ConsoleMessage::Level level, const QString &text);

/// The opaque block behind a custom-classed object, or nullptr when the object is
/// not one of ours.
///
/// This wraps JS_GetAnyOpaque rather than calling it directly, because that
/// function takes the class id as an *out* parameter and writes through it: it
/// cannot be passed nullptr, which is easy to get wrong and crashes rather than
/// failing loudly.
void *opaqueOf(JSValueConst value);

/// The wrapper behind a value, or nullptr when it is not a wrapped node.
Wrapper *wrapperOf(JSValueConst value);

/// The same thing under the name Bindings exposes, so the two do not drift.
static_assert(sizeof(Wrapper) == sizeof(Bindings::WrapperState),
              "Wrapper and Bindings::WrapperState must agree");

/// The node behind `this`, throwing when the receiver is not a node.
dom::Node *thisNode(JSContext *context, JSValueConst thisValue);

/// The element behind `this`, throwing when the receiver is not an element.
dom::Element *thisElement(JSContext *context, JSValueConst thisValue);

/// The document behind `this`, falling back to the context's document.
dom::Document *thisDocument(JSContext *context, JSValueConst thisValue);

/// A class id for an auxiliary object, allocated once per runtime. The bindings
/// need several small object kinds - classList, style, and the like - and giving
/// each a class is what gives it a finalizer, so the small block it carries is
/// freed when the object is collected.
JSClassID auxiliaryClassId(JSContext *context, const char *name, JSClassFinalizer *finalizer);

/// Builds the object a `classList` member returns. A new object per access is
/// cheap: it holds a pointer and a handful of members, and the alternative is a
/// per-element cache that has to be invalidated when the element changes.
JSValue makeClassListObject(JSContext *context, dom::Element *element);

/// Builds the object a `style` member returns.
JSValue makeStyleDeclarationObject(JSContext *context, dom::Element *element);

/// The classList and style objects behind a receiver, or JS_EXCEPTION.
JSValue classListOf(JSContext *context, JSValueConst thisValue);
JSValue styleDeclarationOf(JSContext *context, JSValueConst thisValue);

/// Installs the refused attributes - href, src, value and the rest - onto the
/// element prototype.
void installReflectedAttributes(JSContext *context, JSValue prototype);

/// The node name a browser reports, upper-cased for HTML elements.
QString nodeNameOf(const dom::Node *node);

/// A DOM node's nodeType, using the DOM standard's numbering.
int nodeTypeOf(const dom::Node *node);

/// The wrapper for a node, creating one when needed. Cached, so a node has one
/// wrapper for as long as the context lives.
JSValue wrapNode(JSContext *context, dom::Node *node);

/// Builds a JavaScript string from text.
JSValue newString(JSContext *context, const QString &text);

/// The document the bindings expose, for the installation path.
dom::Document *documentOf(JSContext *context);

/// Reads a JavaScript value as text, the way the DOM APIs do.
QString stringValue(JSContext *context, JSValueConst value);

/// Reads an integer from a JavaScript value.
int intValue(JSContext *context, JSValueConst value);

/// Reads a boolean from a JavaScript value.
bool boolValue(JSContext *context, JSValueConst value);

/// Defines a getter on an object.
void defineGetter(JSContext *context, JSValueConst object, const char *name, JSCFunction *getter);

/// Defines a getter and setter together. `setter` may be null for a read-only
/// member, so that assigning to it does nothing, as in a browser.
void defineAccessor(JSContext *context, JSValueConst object, const char *name,
                    JSCFunction *getter, JSCFunction *setter);

/// Defines a method, with the arity script sees.
void defineMethod(JSContext *context, JSValueConst object, const char *name,
                  JSCFunction *function, int length);

/// Defines a magic-numbered method, used where several functions share one
/// implementation and differ only by a constant.
void defineMagicMethod(JSContext *context, JSValueConst object, const char *name,
                       JSCFunctionMagic *function, int length, int magic);

/// Defines a read-only integer property, for constants.
void defineInt(JSContext *context, JSValueConst object, const char *name, int value);

/// Defines a read-only string property.
void defineString(JSContext *context, JSValueConst object, const char *name,
                  const QString &value);

/// Marks the document as changed, so the browser re-lays it out before painting.
void touchDocument(JSContext *context);

/// Throws a DOMException-shaped error and returns JS_EXCEPTION, which is the one
/// expression every failing binding returns.
JSValue throwDomError(JSContext *context, const QString &name, const QString &message);

// Installed by each bindings translation unit into the objects built by
// Bindings::install().
void installNodeMembers(JSContext *context, JSValue prototype);
void installElementMembers(JSContext *context, JSValue prototype);
void installDocumentMembers(JSContext *context, JSValue prototype);
void installGlobalMembers(JSContext *context, JSValue global, dom::Document *document);
void installTimerMembers(JSContext *context, JSValue global);
void installConsole(JSContext *context, JSValue global);

} // namespace oqb::javascript::detail

#endif // OPENQBROWSER_SCRIPTING
