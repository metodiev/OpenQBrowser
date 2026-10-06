#pragma once

#include <QString>

#ifdef OPENQBROWSER_SCRIPTING
#include <quickjs.h>
#endif

namespace oqb::dom {
class Node;
}

namespace oqb::javascript {

/// DOM events: addEventListener, dispatchEvent, and the event objects a page
/// receives.
///
/// The listener registry lives here rather than on the nodes, because the DOM
/// classes are also used by the parser and the renderer, which have no business
/// knowing that JavaScript exists. One registry is kept per context, so that two
/// pages in two tabs cannot see each other's listeners.
///
/// Dispatch follows the DOM standard's walk: the propagation path is collected
/// from the target up to the root, capture listeners run from the root down, then
/// the target's own listeners, then bubble listeners back up. stopPropagation
/// ends the walk, and stopImmediatePropagation also skips the listeners left on
/// the node being dispatched.
///
/// See architecture/javascript.md for how this fits the rest of the browser.
class Events
{
public:
#ifdef OPENQBROWSER_SCRIPTING
    /// `target.addEventListener(type, listener, options)`.
    static JSValue addListener(JSContext *context, JSValueConst thisValue, int argc,
                               JSValueConst *argv);

    /// `target.removeEventListener(...)`. Matching is by handler identity and by
    /// capture flag, as in a browser: a different function with the same body
    /// does not remove the listener.
    static JSValue removeListener(JSContext *context, JSValueConst thisValue, int argc,
                                  JSValueConst *argv);

    /// `target.dispatchEvent(event)`; false when a cancelable event was
    /// cancelled.
    static JSValue dispatchFromScript(JSContext *context, JSValueConst thisValue, int argc,
                                      JSValueConst *argv);

    /// Builds an event object, for the browser's own events and for the
    /// constructors script calls.
    static JSValue makeEvent(JSContext *context, const QString &type, bool bubbles,
                             bool cancelable);

    /// Builds an event carrying a `detail`, which CustomEvent provides.
    static JSValue makeEventWithDetail(JSContext *context, const QString &type,
                                       JSValueConst detail, bool bubbles, bool cancelable);

    /// Delivers an event the browser created, such as DOMContentLoaded or load.
    /// Returns false when a cancelable event was cancelled.
    static bool dispatch(JSContext *context, dom::Node *target, const QString &type, bool bubbles,
                         bool cancelable);

    /// Installs the Event prototype's members onto `eventPrototype`.
    static void installMembers(JSContext *context, JSValue eventPrototype);

    /// The finalizer the event class is registered with. An event object owns
    /// one small block of data, so without this every dispatched event leaks it.
    static JSClassFinalizer *finalizer();

    /// Installs the Event, CustomEvent, MouseEvent and KeyboardEvent
    /// constructors as globals.
    static void installConstructors(JSContext *context, JSValue global);

    /// Registers a listener on the page's window, which is the global object
    /// rather than a node. Pages put DOMContentLoaded and load listeners on
    /// window, so these have to reach the same dispatch the document uses.
    static JSValue addWindowListener(JSContext *context, JSValueConst thisValue, int argc,
                                     JSValueConst *argv);

    /// Removes a listener registered on the page's window.
    static JSValue removeWindowListener(JSContext *context, JSValueConst thisValue, int argc,
                                        JSValueConst *argv);

    /// Dispatches a lifecycle event to the document and to the window listeners,
    /// which is what the browser calls for DOMContentLoaded and load.
    static void dispatchLifecycle(JSContext *context, dom::Node *document, const QString &type,
                                  bool bubbles, bool cancelable);

    /// Releases every listener value. Must be called before the context is
    /// freed, because a value outliving its context is a use-after-free.
    static void clearContext(JSContext *context);
#endif
};

} // namespace oqb::javascript
