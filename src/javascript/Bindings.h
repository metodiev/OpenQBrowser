#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <memory>

#include "javascript/ScriptEngine.h"

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::dom {
class Document;
class Node;
}

namespace oqb::javascript {

class TimerQueue;
class EventTarget;

/// The bridge between QuickJS and OpenQBrowser's document.
///
/// ## Ownership
///
/// The DOM owns its nodes through `std::unique_ptr`, but script holds references
/// that outlive a node's removal from the tree:
///
///   var el = document.getElementById("x");
///   el.remove();
///   el.textContent = "still works";
///
/// A single rule makes that safe: every node that is not in the document tree is
/// owned by the context's graveyard for as long as the context lives. A node is
/// therefore owned by exactly one of the tree or the graveyard, never both and
/// never by a wrapper. Detaching a node moves it to the graveyard; inserting one
/// takes it back out. Nothing is freed while script could reach it, which trades
/// a bounded amount of memory for the absence of use-after-free. The graveyard is
/// emptied when the engine's runtime is destroyed.
///
/// Each node also has at most one wrapper object, so `el === document.body` holds
/// and the same object comes back from every API, as in a browser.
///
/// The bindings expose the DOM as ordinary JavaScript objects with accessor
/// properties rather than as free functions, so page code written against a real
/// browser works unchanged:
///
///   document.getElementById("x").classList.add("on");
///   el.textContent = "hi";
///   el.style.color = "red";
///   el.addEventListener("click", handler);
///
/// See architecture/javascript.md.
class Bindings
{
public:
#ifdef OPENQBROWSER_SCRIPTING
    /// The class ids QuickJS registers the wrapper classes under. They are
    /// allocated once per runtime and every wrapper of that kind shares one.
    struct ClassIds
    {
        JSClassID node = 0;
        JSClassID element = 0;
        JSClassID document = 0;
        JSClassID event = 0;
        bool valid = false;
    };

    /// Creates the global object and installs window, document, console and the
    /// timer functions. Returns the global, which the engine keeps a reference
    /// to. The result is JS_EXCEPTION if a wrapper class could not be registered.
    static JSValue install(JSContext *context, ClassIds &classes, dom::Document *document,
                           TimerQueue *timers, QList<ConsoleMessage> *messages);

    /// Points `document` at a new document, which a navigation needs without
    /// rebuilding the runtime and recompiling the built-ins.
    static void setDocument(JSContext *context, dom::Document *document);

    /// Wraps `node` for script. Pass `owned` when the wrapper is taking
    /// ownership, which is the case for a node script has detached.
    static JSValue wrapNode(JSContext *context, dom::Node *node, bool owned = false);

    /// The node behind a wrapper, or nullptr when `value` is not a node.
    static dom::Node *nodeOf(JSValueConst value);

    /// Releases everything the bindings hold for a context. Must run before the
    /// runtime is freed, because the values and the detached nodes belong to it.
    static void destroy(JSContext *context);

    /// Takes a node out of the tree and gives it to the graveyard, so that any
    /// wrapper script holds keeps working. Does nothing when it is already out.
    static void detachNode(JSContext *context, dom::Node *node);

    /// Takes a node back out of the graveyard, transferring ownership to the
    /// caller. Null when the node is not there, meaning the tree still owns it.
    static std::unique_ptr<dom::Node> adoptNode(JSContext *context, dom::Node *node);

    /// Puts a freshly created, still-detached node under the graveyard's
    /// ownership, so that it has an owner from the moment it exists.
    static void ownNewNode(JSContext *context, dom::Node *node);

    /// The document the bindings currently expose.
    static dom::Document *documentOf(JSContext *context);

    /// The class ids in use for a context.
    static const ClassIds &classIdsOf(JSContext *context);

    /// Records that script changed the document, so the browser re-styles and
    /// re-lays it out before painting. Every mutating binding calls this.
    static void touchDocument(JSContext *context);

    /// True when the document changed since this was last asked, clearing the
    /// flag. The browser uses it to decide whether a re-layout is needed.
    static bool takeDocumentTouched(JSContext *context);

    /// Builds a JavaScript string. Used by the other binding translation units.
    static JSValue newString(JSContext *context, const QString &text);

    /// Reads a JavaScript value as text, the way the DOM APIs do.
    static QString stringValue(JSContext *context, JSValueConst value);

    /// Reads an integer property, for element indices and offsets.
    static int intProperty(JSContext *context, JSValueConst object, const char *name);

    /// The prototype object for a wrapper kind: "node", "element", "document" or
    /// "event". Used when a binding has to construct a related object.
    static JSValue prototype(JSContext *context, const QString &kind);

    /// What a wrapper object holds. Provided for the event bindings, which need
    /// the node behind a value they were handed.
    struct WrapperState
    {
        dom::Node *node = nullptr;
    };
    /// Returns the wrapper behind a value, creating none. Null when `value` is
    /// not a wrapped node.
    static WrapperState *wrapperState(JSValueConst value);

    /// Throws a DOMException-shaped error and returns JS_EXCEPTION, which is the
    /// single expression every failing binding returns.
    static JSValue throwDomError(JSContext *context, const QString &name,
                                 const QString &message);
#endif

private:
    Bindings() = delete;
};

} // namespace oqb::javascript
