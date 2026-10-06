#include "javascript/BindingsInternal.h"

#include "javascript/Events.h"

#ifdef OPENQBROWSER_SCRIPTING

namespace oqb::javascript::detail {
namespace {

// Document-order traversal of a subtree including the root, used by the
// querying members so that `querySelector` on a detached subtree still works.
void collectElements(dom::Node *node, bool includeRoot, QList<dom::Element *> *out)
{
    if (!node)
        return;
    if (node->isElement() && includeRoot)
        out->append(static_cast<dom::Element *>(node));
    for (const auto &child : node->children())
        collectElements(child.get(), true, out);
}

// ------------------------------------------------------------------- Node

JSValue nodeGetNodeName(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return newString(context, nodeNameOf(node));
    return JS_EXCEPTION;
}

JSValue nodeGetNodeType(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return JS_NewInt32(context, nodeTypeOf(node));
    return JS_EXCEPTION;
}

JSValue nodeGetNodeValue(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    // Only text and comment nodes carry a value; an element's is null, which the
    // DOM standard specifies and which a lot of script tests for.
    if (node->isText())
        return newString(context, static_cast<dom::Text *>(node)->data());
    if (node->isComment())
        return newString(context, static_cast<dom::Comment *>(node)->data());
    return JS_NULL;
}

JSValue nodeSetNodeValue(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    // Elements have no settable node value, so the assignment is a no-op rather
    // than an error, matching a browser.
    if (argc > 0 && node->isText()) {
        static_cast<dom::Text *>(node)->setData(stringValue(context, argv[0]));
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

JSValue nodeGetTextContent(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return newString(context, node->textContent());
    return JS_EXCEPTION;
}

JSValue nodeSetTextContent(JSContext *context, JSValueConst thisValue, int argc,
                           JSValueConst *argv)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_UNDEFINED;

    if (node->isText()) {
        // Setting textContent on a text node replaces its data rather than its
        // children, which the standard calls out explicitly.
        static_cast<dom::Text *>(node)->setData(stringValue(context, argv[0]));
    } else {
        dom::Document *document = node->ownerDocument();
        if (!document)
            return JS_UNDEFINED;

        // Replacing content must not invalidate references script already holds,
        // so the old children are moved aside rather than destroyed.
        clearChildrenKeepingScript(context, node);
        node->appendChild(document->createTextNode(stringValue(context, argv[0])));
    }

    touchDocument(context);
    return JS_UNDEFINED;
}

JSValue nodeGetParentNode(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->parent());
    return JS_EXCEPTION;
}

JSValue nodeGetParentElement(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->parentElement());
    return JS_EXCEPTION;
}

/// The live child list, built as a JavaScript array. A real NodeList would be
/// live, but the difference is only visible to script that mutates the tree
/// while iterating a collection, and an array keeps the spread and
/// forEach patterns working.
JSValue childArray(JSContext *context, dom::Node *node)
{
    JSValue array = JS_NewArray(context);
    uint32_t index = 0;
    for (const auto &child : node->children())
        JS_SetPropertyUint32(context, array, index++, wrapNode(context, child.get()));
    return array;
}

JSValue nodeGetChildNodes(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return childArray(context, node);
    return JS_EXCEPTION;
}

JSValue nodeGetFirstChild(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->firstChild());
    return JS_EXCEPTION;
}

JSValue nodeGetLastChild(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->lastChild());
    return JS_EXCEPTION;
}

JSValue nodeGetNextSibling(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->nextSibling());
    return JS_EXCEPTION;
}

JSValue nodeGetPreviousSibling(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->previousSibling());
    return JS_EXCEPTION;
}

JSValue nodeGetOwnerDocument(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return wrapNode(context, node->ownerDocument());
    return JS_EXCEPTION;
}

/// Reads a node argument, reporting a TypeError when it is not one.
dom::Node *nodeArgument(JSContext *context, JSValueConst value, const char *what)
{
    if (dom::Node *node = Bindings::nodeOf(value))
        return node;

    throwDomError(context, QStringLiteral("TypeError"),
                  QStringLiteral("the %1 is not a node").arg(QString::fromLatin1(what)));
    return nullptr;
}

/// Inserts `child` into `parent`, taking it from wherever it currently is.
///
/// This is the single place the tree/graveyard hand-off happens for insertion,
/// which is what keeps the ownership rule true: the node leaves the graveyard if
/// it is there, and leaves its old parent if it has one.
bool insertNode(JSContext *context, dom::Node *parent, dom::Node *child, dom::Node *reference)
{
    // A node cannot contain itself, and a node cannot be inserted into its own
    // descendant: either would make the tree a cycle and hang every later walk.
    for (dom::Node *ancestor = parent; ancestor; ancestor = ancestor->parent()) {
        if (ancestor == child) {
            throwDomError(context, QStringLiteral("HierarchyRequestError"),
                          QStringLiteral("the node would become its own ancestor"));
            return false;
        }
    }

    if (child->parent() == parent && !reference) {
        // Already where it is being put, so there is nothing to do. Without this
        // the detach below would move it to the end when it was not there.
        if (parent->lastChild() == child)
            return true;
    } else if (child->parent() == parent && reference) {
        // Moving within one parent: the node is taken out first so that
        // insertBefore's index is computed against the list without it.
        if (child->nextSibling() == reference)
            return true;
    }

    std::unique_ptr<dom::Node> owned;

    if (child->parent()) {
        // The tree it is leaving hands ownership back to us.
        owned = child->parent()->removeChild(child);
    } else {
        // It was detached, so the graveyard is holding it.
        owned = Bindings::adoptNode(context, child);
    }

    if (!owned) {
        // Neither the tree nor the graveyard owns it, which can only mean it is
        // already a child of the target and was left in place above.
        return true;
    }

    if (reference)
        parent->insertBefore(std::move(owned), reference);
    else
        parent->appendChild(std::move(owned));

    return true;
}

JSValue nodeAppendChild(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *parent = thisNode(context, thisValue);
    if (!parent)
        return JS_EXCEPTION;

    if (argc < 1) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("appendChild needs a node"));
    }

    dom::Node *child = nodeArgument(context, argv[0], "argument");
    if (!child)
        return JS_EXCEPTION;

    if (!insertNode(context, parent, child, nullptr))
        return JS_EXCEPTION;

    touchDocument(context);
    return JS_DupValue(context, argv[0]);
}

JSValue nodeInsertBefore(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *parent = thisNode(context, thisValue);
    if (!parent)
        return JS_EXCEPTION;

    if (argc < 2) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("insertBefore needs a node and a reference"));
    }

    dom::Node *child = nodeArgument(context, argv[0], "argument");
    if (!child)
        return JS_EXCEPTION;

    // A null reference means "append", which the standard allows so that
    // insertBefore can be called unconditionally.
    dom::Node *reference = nullptr;
    if (!JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
        reference = nodeArgument(context, argv[1], "reference");
        if (!reference)
            return JS_EXCEPTION;

        if (reference->parent() != parent) {
            return throwDomError(context, QStringLiteral("NotFoundError"),
                                 QStringLiteral("the reference is not a child of this node"));
        }
    }

    if (!insertNode(context, parent, child, reference))
        return JS_EXCEPTION;

    touchDocument(context);
    return JS_DupValue(context, argv[0]);
}

JSValue nodeRemoveChild(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *parent = thisNode(context, thisValue);
    if (!parent)
        return JS_EXCEPTION;

    if (argc < 1) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("removeChild needs a node"));
    }

    dom::Node *child = nodeArgument(context, argv[0], "argument");
    if (!child)
        return JS_EXCEPTION;

    if (child->parent() != parent) {
        return throwDomError(context, QStringLiteral("NotFoundError"),
                             QStringLiteral("the node is not a child of this node"));
    }

    // The graveyard takes ownership, so script keeps working with the node it
    // just removed.
    detachFromTree(context, child);

    touchDocument(context);
    return JS_DupValue(context, argv[0]);
}

JSValue nodeReplaceChild(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *parent = thisNode(context, thisValue);
    if (!parent)
        return JS_EXCEPTION;

    if (argc < 2) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("replaceChild needs two nodes"));
    }

    dom::Node *replacement = nodeArgument(context, argv[0], "replacement");
    if (!replacement)
        return JS_EXCEPTION;

    dom::Node *oldNode = nodeArgument(context, argv[1], "node to replace");
    if (!oldNode)
        return JS_EXCEPTION;

    if (oldNode->parent() != parent) {
        return throwDomError(
            context, QStringLiteral("NotFoundError"),
            QStringLiteral("the node to replace is not a child of this node"));
    }

    // The replacement goes in first, so the old node still has a position to be
    // taken out of afterwards.
    if (!insertNode(context, parent, replacement, oldNode))
        return JS_EXCEPTION;

    detachFromTree(context, oldNode);

    touchDocument(context);
    return JS_DupValue(context, argv[1]);
}

/// Copies one node shallowly: element attributes, text and comments. These are
/// the kinds a page clones in practice.
std::unique_ptr<dom::Node> shallowClone(const dom::Node *node, dom::Document *document)
{
    if (node->isElement()) {
        const auto *element = static_cast<const dom::Element *>(node);
        auto copy = document->createElement(element->tagName());
        for (const auto &attribute : element->attr().entries())
            copy->attr().append(attribute.first, attribute.second);
        return copy;
    }
    if (node->isText())
        return document->createTextNode(static_cast<const dom::Text *>(node)->data());
    if (node->isComment())
        return document->createComment(static_cast<const dom::Comment *>(node)->data());
    return nullptr;
}

JSValue nodeCloneNode(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    dom::Document *document = node->ownerDocument();
    if (!document)
        return JS_NULL;

    const bool deep = argc > 0 && boolValue(context, argv[0]);

    std::unique_ptr<dom::Node> clone = shallowClone(node, document);
    if (!clone)
        return JS_NULL;

    if (deep) {
        // The subtree is copied so a cloned container arrives with its content,
        // which is what cloneNode(true) promises.
        std::function<void(const dom::Node *, dom::Node *)> copyChildren
            = [&](const dom::Node *from, dom::Node *to) {
                  for (const auto &child : from->children()) {
                      std::unique_ptr<dom::Node> childClone = shallowClone(child.get(), document);
                      if (!childClone)
                          continue;
                      dom::Node *raw = to->appendChild(std::move(childClone));
                      copyChildren(child.get(), raw);
                  }
              };
        copyChildren(node, clone.get());
    }

    // A clone is detached from the start, so the graveyard owns it immediately.
    dom::Node *raw = clone.release();
    ownNewNode(context, raw);
    return wrapNode(context, raw);
}

JSValue nodeHasChildNodes(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Node *node = thisNode(context, thisValue))
        return JS_NewBool(context, node->childCount() > 0);
    return JS_EXCEPTION;
}

JSValue nodeContains(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    // `node.contains(node)` is true, which is why the walk starts at the node
    // the argument names rather than at its parent.
    dom::Node *other = argc > 0 ? Bindings::nodeOf(argv[0]) : nullptr;
    for (dom::Node *current = other; current; current = current->parent()) {
        if (current == node)
            return JS_NewBool(context, true);
    }
    return JS_NewBool(context, false);
}

JSValue nodeAddEventListener(JSContext *context, JSValueConst thisValue, int argc,
                             JSValueConst *argv)
{
    return Events::addListener(context, thisValue, argc, argv);
}

JSValue nodeRemoveEventListener(JSContext *context, JSValueConst thisValue, int argc,
                                JSValueConst *argv)
{
    return Events::removeListener(context, thisValue, argc, argv);
}

JSValue nodeDispatchEvent(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    return Events::dispatchFromScript(context, thisValue, argc, argv);
}

JSValue nodeRemove(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Node *node = thisNode(context, thisValue);
    if (!node)
        return JS_EXCEPTION;

    if (node->parent()) {
        detachFromTree(context, node);
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- Element

JSValue elementGetTagName(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return newString(context, element->tagName().toUpper());
    return JS_EXCEPTION;
}

JSValue elementGetLocalName(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return newString(context, element->localName());
    return JS_EXCEPTION;
}

JSValue elementGetId(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return newString(context, element->id());
    return JS_EXCEPTION;
}

JSValue elementSetId(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc > 0) {
        element->attr().set(QStringLiteral("id"), stringValue(context, argv[0]));
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

JSValue elementGetClassName(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return newString(context, element->className());
    return JS_EXCEPTION;
}

JSValue elementSetClassName(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc > 0) {
        element->attr().set(QStringLiteral("class"), stringValue(context, argv[0]));
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

JSValue elementGetAttribute(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("getAttribute needs a name"));
    }

    // An absent attribute is null rather than an empty string, which is what the
    // standard specifies and what script tests with `!== null`.
    const QString name = stringValue(context, argv[0]).toLower();
    if (!element->hasAttribute(name))
        return JS_NULL;
    return newString(context, element->attribute(name));
}

JSValue elementSetAttribute(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 2) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("setAttribute needs a name and a value"));
    }

    const QString name = stringValue(context, argv[0]).toLower();
    if (name.isEmpty()) {
        return throwDomError(context, QStringLiteral("InvalidCharacterError"),
                             QStringLiteral("the attribute name is empty"));
    }

    element->attr().set(name, stringValue(context, argv[1]));
    touchDocument(context);
    return JS_UNDEFINED;
}

JSValue elementRemoveAttribute(JSContext *context, JSValueConst thisValue, int argc,
                               JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc > 0) {
        element->attr().remove(stringValue(context, argv[0]).toLower());
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

JSValue elementHasAttribute(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewBool(context, false);
    return JS_NewBool(context, element->hasAttribute(stringValue(context, argv[0]).toLower()));
}

JSValue elementGetInnerHTML(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    QString html;
    for (const auto &child : element->children())
        html += child->toHtml();
    return newString(context, html);
}

JSValue elementSetInnerHTML(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_UNDEFINED;

    dom::Document *document = element->ownerDocument();
    if (!document)
        return JS_UNDEFINED;

    // The existing children are moved aside rather than destroyed, so a
    // reference script is holding stays usable.
    clearChildrenKeepingScript(context, element);

    // Parsed with this element's tag as the context, so setting innerHTML on a
    // table row produces cells rather than stray content.
    const QList<dom::Node *> parsed
        = html::Parser::parseFragment(stringValue(context, argv[0]), document, element->tagName());
    for (dom::Node *node : parsed)
        element->appendChild(std::unique_ptr<dom::Node>(node));

    touchDocument(context);
    return JS_UNDEFINED;
}

JSValue elementGetOuterHTML(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;
    return newString(context, element->toHtml());
}

JSValue elementSetOuterHTML(JSContext *context, JSValueConst thisValue, int argc,
                            JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    dom::Node *parent = element->parent();
    if (!parent || argc < 1)
        return JS_UNDEFINED;

    // A fragment is used as the insertion point so that setting outerHTML to
    // markup with several roots inserts all of them, as the standard requires.
    dom::Document *document = element->ownerDocument();
    if (!document)
        return JS_UNDEFINED;

    const QList<dom::Node *> parsed = html::Parser::parseFragment(
        stringValue(context, argv[0]), document, parent->isElement()
                                                     ? static_cast<dom::Element *>(parent)->tagName()
                                                     : QStringLiteral("div"));

    // Each new node goes before the element being replaced; without this the
    // order would come out reversed.
    for (dom::Node *node : parsed)
        insertNode(context, parent, node, element);

    detachFromTree(context, element);

    touchDocument(context);
    return JS_UNDEFINED;
}

JSValue elementGetChildren(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    JSValue array = JS_NewArray(context);
    uint32_t index = 0;
    for (dom::Element *child : element->childElements())
        JS_SetPropertyUint32(context, array, index++, wrapNode(context, child));
    return array;
}

JSValue elementGetChildElementCount(JSContext *context, JSValueConst thisValue, int,
                                    JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return JS_NewInt32(context, static_cast<int>(element->childElements().size()));
    return JS_EXCEPTION;
}

JSValue elementGetFirstElementChild(JSContext *context, JSValueConst thisValue, int,
                                    JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return wrapNode(context, element->firstElementChild());
    return JS_EXCEPTION;
}

JSValue elementGetLastElementChild(JSContext *context, JSValueConst thisValue, int,
                                   JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return wrapNode(context, element->lastElementChild());
    return JS_EXCEPTION;
}

JSValue elementGetNextElementSibling(JSContext *context, JSValueConst thisValue, int,
                                     JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return wrapNode(context, element->nextElementSibling());
    return JS_EXCEPTION;
}

JSValue elementGetPreviousElementSibling(JSContext *context, JSValueConst thisValue, int,
                                         JSValueConst *)
{
    if (dom::Element *element = thisElement(context, thisValue))
        return wrapNode(context, element->previousElementSibling());
    return JS_EXCEPTION;
}

JSValue elementGetClassList(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    return classListOf(context, thisValue);
}

JSValue elementGetStyle(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    return styleDeclarationOf(context, thisValue);
}

// --------------------------------------------------------------- querying

/// Matches a candidate against a selector list: the shared core of every
/// querying member and of matches()/closest().
bool matchesAny(const QList<css::Selector> &selectors, const dom::Element *element)
{
    for (const css::Selector &selector : selectors) {
        if (selector.matches(element))
            return true;
    }
    return false;
}

QList<css::Selector> selectorsFrom(JSContext *context, JSValueConst value)
{
    return css::SelectorParser::parseList(stringValue(context, value));
}

JSValue matchedIntoArray(JSContext *context, const QList<dom::Element *> &elements)
{
    JSValue array = JS_NewArray(context);
    uint32_t index = 0;
    for (dom::Element *element : elements)
        JS_SetPropertyUint32(context, array, index++, wrapNode(context, element));
    return array;
}

JSValue elementQuerySelector(JSContext *context, JSValueConst thisValue, int argc,
                             JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NULL;

    const QList<css::Selector> selectors = selectorsFrom(context, argv[0]);

    // The search is over descendants only, so the element itself is not a
    // candidate, which is what querySelector specifies.
    QList<dom::Element *> candidates;
    collectElements(element, false, &candidates);
    for (dom::Element *candidate : candidates) {
        if (matchesAny(selectors, candidate))
            return wrapNode(context, candidate);
    }
    return JS_NULL;
}

JSValue elementQuerySelectorAll(JSContext *context, JSValueConst thisValue, int argc,
                                JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    const QList<css::Selector> selectors = selectorsFrom(context, argv[0]);

    QList<dom::Element *> candidates;
    collectElements(element, false, &candidates);

    QList<dom::Element *> matched;
    for (dom::Element *candidate : candidates) {
        if (matchesAny(selectors, candidate))
            matched.append(candidate);
    }
    return matchedIntoArray(context, matched);
}

/// The by-name lookups share a body: they differ only in how a candidate is
/// tested, so the predicate is passed in.
template <typename Predicate>
JSValue elementsWhere(JSContext *context, dom::Element *element, Predicate predicate)
{
    QList<dom::Element *> candidates;
    collectElements(element, false, &candidates);

    QList<dom::Element *> matched;
    for (dom::Element *candidate : candidates) {
        if (predicate(candidate))
            matched.append(candidate);
    }
    return matchedIntoArray(context, matched);
}

JSValue elementGetElementsByTagName(JSContext *context, JSValueConst thisValue, int argc,
                                    JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    const QString tag = stringValue(context, argv[0]);
    const bool any = tag == QLatin1String("*");
    return elementsWhere(context, element,
                         [&](dom::Element *candidate) { return any || candidate->isTag(tag); });
}

JSValue elementGetElementsByClassName(JSContext *context, JSValueConst thisValue, int argc,
                                      JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    // The argument is a whitespace-separated list and an element matches when it
    // carries all of the names, which is what the standard specifies.
    const QStringList wanted = stringValue(context, argv[0]).split(u' ', Qt::SkipEmptyParts);
    return elementsWhere(context, element, [&](dom::Element *candidate) {
        for (const QString &name : wanted) {
            if (!candidate->classList().contains(name))
                return false;
        }
        return !wanted.isEmpty();
    });
}

JSValue elementClosest(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NULL;

    const QList<css::Selector> selectors = selectorsFrom(context, argv[0]);

    // closest walks up from the element itself, so it can return the receiver.
    for (dom::Element *candidate = element; candidate; candidate = candidate->parentElement()) {
        if (matchesAny(selectors, candidate))
            return wrapNode(context, candidate);
    }
    return JS_NULL;
}

JSValue elementMatches(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Element *element = thisElement(context, thisValue);
    if (!element)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewBool(context, false);
    return JS_NewBool(context, matchesAny(selectorsFrom(context, argv[0]), element));
}

/// getBoundingClientRect. The geometry lives in the renderer, which the DOM
/// bindings cannot reach, so this reports a zero rect rather than inventing
/// numbers that would be wrong after any layout change.
JSValue elementGetBoundingClientRect(JSContext *context, JSValueConst thisValue, int,
                                     JSValueConst *)
{
    if (!thisElement(context, thisValue))
        return JS_EXCEPTION;

    JSValue rect = JS_NewObject(context);
    for (const char *field : {"x", "y", "width", "height", "top", "right", "bottom", "left"})
        JS_DefinePropertyValueStr(context, rect, field, JS_NewInt32(context, 0), JS_PROP_C_W_E);
    return rect;
}

// ---------------------------------------------------------------- Document

JSValue documentGetElementById(JSContext *context, JSValueConst thisValue, int argc,
                               JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NULL;
    return wrapNode(context, document->getElementById(stringValue(context, argv[0])));
}

JSValue documentCreateElement(JSContext *context, JSValueConst thisValue, int argc,
                              JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1) {
        return throwDomError(context, QStringLiteral("TypeError"),
                             QStringLiteral("createElement needs a tag name"));
    }

    const QString tag = stringValue(context, argv[0]);
    if (tag.isEmpty()) {
        return throwDomError(context, QStringLiteral("InvalidCharacterError"),
                             QStringLiteral("the tag name is empty"));
    }

    // Created detached, so the graveyard owns it from the start and a wrapper
    // that script keeps stays valid even if it is never inserted.
    dom::Node *raw = document->createElement(tag).release();
    ownNewNode(context, raw);
    return wrapNode(context, raw);
}

JSValue documentCreateTextNode(JSContext *context, JSValueConst thisValue, int argc,
                               JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    const QString data = argc > 0 ? stringValue(context, argv[0]) : QString();
    dom::Node *raw = document->createTextNode(data).release();
    ownNewNode(context, raw);
    return wrapNode(context, raw);
}

JSValue documentCreateComment(JSContext *context, JSValueConst thisValue, int argc,
                              JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    const QString data = argc > 0 ? stringValue(context, argv[0]) : QString();
    dom::Node *raw = document->createComment(data).release();
    ownNewNode(context, raw);
    return wrapNode(context, raw);
}

JSValue documentCreateDocumentFragment(JSContext *context, JSValueConst thisValue, int,
                                       JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    // A fragment is a real detached container, so inserting it moves its
    // children as a group only if the page moves them individually. Pages that
    // matter append it and then read children, which this supports.
    dom::Node *raw = document->createElement(QStringLiteral("#fragment")).release();
    ownNewNode(context, raw);
    return wrapNode(context, raw);
}

JSValue documentGetDocumentElement(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;
    return wrapNode(context, document->documentElement());
}

JSValue documentGetHead(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;
    return wrapNode(context, document->head());
}

JSValue documentGetBody(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;
    return wrapNode(context, document->body());
}

JSValue documentGetTitle(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;
    return newString(context, document->title());
}

JSValue documentSetTitle(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc > 0) {
        document->setTitle(stringValue(context, argv[0]));
        touchDocument(context);
    }
    return JS_UNDEFINED;
}

JSValue documentGetUrl(JSContext *context, JSValueConst thisValue, int, JSValueConst *)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;
    return newString(context, document->url().toString());
}

JSValue documentGetReadyState(JSContext *context, JSValueConst, int, JSValueConst *)
{
    // The load state is kept on the global, because the script runner is what
    // knows how far through the document it is.
    JSValue global = JS_GetGlobalObject(context);
    JSValue state = JS_GetPropertyStr(context, global, "__readyState");
    JS_FreeValue(context, global);

    if (JS_IsUndefined(state)) {
        JS_FreeValue(context, state);
        return newString(context, QStringLiteral("loading"));
    }
    return state;
}

JSValue documentWrite(JSContext *context, JSValueConst thisValue, int argc, JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    dom::Element *body = document->body();
    if (!body)
        return JS_UNDEFINED;

    QString html;
    for (int i = 0; i < argc; ++i)
        html += stringValue(context, argv[i]);

    const QList<dom::Node *> parsed = html::Parser::parseFragment(html, document);
    for (dom::Node *node : parsed)
        body->appendChild(std::unique_ptr<dom::Node>(node));

    touchDocument(context);
    return JS_UNDEFINED;
}

// The document's querying members search the whole tree, so they cannot reuse
// the element ones, which start below the receiver.
JSValue documentQuerySelector(JSContext *context, JSValueConst thisValue, int argc,
                              JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NULL;

    const QList<css::Selector> selectors = selectorsFrom(context, argv[0]);

    QList<dom::Element *> candidates;
    collectElements(document, true, &candidates);
    for (dom::Element *candidate : candidates) {
        if (matchesAny(selectors, candidate))
            return wrapNode(context, candidate);
    }
    return JS_NULL;
}

JSValue documentQuerySelectorAll(JSContext *context, JSValueConst thisValue, int argc,
                                 JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    const QList<css::Selector> selectors = selectorsFrom(context, argv[0]);

    QList<dom::Element *> candidates;
    collectElements(document, true, &candidates);

    QList<dom::Element *> matched;
    for (dom::Element *candidate : candidates) {
        if (matchesAny(selectors, candidate))
            matched.append(candidate);
    }
    return matchedIntoArray(context, matched);
}

JSValue documentGetElementsByTagName(JSContext *context, JSValueConst thisValue, int argc,
                                     JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    const QString tag = stringValue(context, argv[0]);
    if (tag == QLatin1String("*"))
        return matchedIntoArray(context, document->getElementsByTagName(QStringLiteral("*")));

    QList<dom::Element *> candidates;
    collectElements(document, true, &candidates);

    QList<dom::Element *> matched;
    for (dom::Element *candidate : candidates) {
        if (candidate->isTag(tag))
            matched.append(candidate);
    }
    return matchedIntoArray(context, matched);
}

JSValue documentGetElementsByClassName(JSContext *context, JSValueConst thisValue, int argc,
                                       JSValueConst *argv)
{
    dom::Document *document = thisDocument(context, thisValue);
    if (!document)
        return JS_EXCEPTION;

    if (argc < 1)
        return JS_NewArray(context);

    const QStringList wanted = stringValue(context, argv[0]).split(u' ', Qt::SkipEmptyParts);

    QList<dom::Element *> candidates;
    collectElements(document, true, &candidates);

    QList<dom::Element *> matched;
    for (dom::Element *candidate : candidates) {
        bool carriersAll = !wanted.isEmpty();
        for (const QString &name : wanted) {
            if (!candidate->classList().contains(name)) {
                carriersAll = false;
                break;
            }
        }
        if (carriersAll)
            matched.append(candidate);
    }
    return matchedIntoArray(context, matched);
}

} // namespace

// ------------------------------------------------------------- installers

void installNodeMembers(JSContext *context, JSValue prototype)
{
    defineGetter(context, prototype, "nodeName", nodeGetNodeName);
    defineGetter(context, prototype, "nodeType", nodeGetNodeType);
    defineAccessor(context, prototype, "nodeValue", nodeGetNodeValue, nodeSetNodeValue);
    defineAccessor(context, prototype, "textContent", nodeGetTextContent, nodeSetTextContent);
    defineGetter(context, prototype, "parentNode", nodeGetParentNode);
    defineGetter(context, prototype, "parentElement", nodeGetParentElement);
    defineGetter(context, prototype, "childNodes", nodeGetChildNodes);
    defineGetter(context, prototype, "firstChild", nodeGetFirstChild);
    defineGetter(context, prototype, "lastChild", nodeGetLastChild);
    defineGetter(context, prototype, "nextSibling", nodeGetNextSibling);
    defineGetter(context, prototype, "previousSibling", nodeGetPreviousSibling);
    defineGetter(context, prototype, "ownerDocument", nodeGetOwnerDocument);

    defineMethod(context, prototype, "appendChild", nodeAppendChild, 1);
    defineMethod(context, prototype, "insertBefore", nodeInsertBefore, 2);
    defineMethod(context, prototype, "removeChild", nodeRemoveChild, 1);
    defineMethod(context, prototype, "replaceChild", nodeReplaceChild, 2);
    defineMethod(context, prototype, "cloneNode", nodeCloneNode, 1);
    defineMethod(context, prototype, "hasChildNodes", nodeHasChildNodes, 0);
    defineMethod(context, prototype, "contains", nodeContains, 1);
    defineMethod(context, prototype, "remove", nodeRemove, 0);
    defineMethod(context, prototype, "addEventListener", nodeAddEventListener, 2);
    defineMethod(context, prototype, "removeEventListener", nodeRemoveEventListener, 2);
    defineMethod(context, prototype, "dispatchEvent", nodeDispatchEvent, 1);

    // The node type constants, which script compares nodeType against.
    defineInt(context, prototype, "ELEMENT_NODE", 1);
    defineInt(context, prototype, "TEXT_NODE", 3);
    defineInt(context, prototype, "COMMENT_NODE", 8);
    defineInt(context, prototype, "DOCUMENT_NODE", 9);
}

void installElementMembers(JSContext *context, JSValue prototype)
{
    defineGetter(context, prototype, "tagName", elementGetTagName);
    defineGetter(context, prototype, "localName", elementGetLocalName);
    defineAccessor(context, prototype, "id", elementGetId, elementSetId);
    defineAccessor(context, prototype, "className", elementGetClassName, elementSetClassName);
    defineGetter(context, prototype, "classList", elementGetClassList);
    defineAccessor(context, prototype, "innerHTML", elementGetInnerHTML, elementSetInnerHTML);
    defineAccessor(context, prototype, "outerHTML", elementGetOuterHTML, elementSetOuterHTML);
    defineGetter(context, prototype, "style", elementGetStyle);
    defineGetter(context, prototype, "children", elementGetChildren);
    defineGetter(context, prototype, "childElementCount", elementGetChildElementCount);
    defineGetter(context, prototype, "firstElementChild", elementGetFirstElementChild);
    defineGetter(context, prototype, "lastElementChild", elementGetLastElementChild);
    defineGetter(context, prototype, "nextElementSibling", elementGetNextElementSibling);
    defineGetter(context, prototype, "previousElementSibling", elementGetPreviousElementSibling);

    defineMethod(context, prototype, "getAttribute", elementGetAttribute, 1);
    defineMethod(context, prototype, "setAttribute", elementSetAttribute, 2);
    defineMethod(context, prototype, "removeAttribute", elementRemoveAttribute, 1);
    defineMethod(context, prototype, "hasAttribute", elementHasAttribute, 1);
    defineMethod(context, prototype, "getBoundingClientRect", elementGetBoundingClientRect, 0);

    defineMethod(context, prototype, "querySelector", elementQuerySelector, 1);
    defineMethod(context, prototype, "querySelectorAll", elementQuerySelectorAll, 1);
    defineMethod(context, prototype, "getElementsByTagName", elementGetElementsByTagName, 1);
    defineMethod(context, prototype, "getElementsByClassName", elementGetElementsByClassName, 1);
    defineMethod(context, prototype, "closest", elementClosest, 1);
    defineMethod(context, prototype, "matches", elementMatches, 1);

    installReflectedAttributes(context, prototype);
}

void installDocumentMembers(JSContext *context, JSValue prototype)
{
    defineMethod(context, prototype, "getElementById", documentGetElementById, 1);
    defineMethod(context, prototype, "getElementsByTagName", documentGetElementsByTagName, 1);
    defineMethod(context, prototype, "getElementsByClassName", documentGetElementsByClassName, 1);
    defineMethod(context, prototype, "querySelector", documentQuerySelector, 1);
    defineMethod(context, prototype, "querySelectorAll", documentQuerySelectorAll, 1);
    defineMethod(context, prototype, "createElement", documentCreateElement, 1);
    defineMethod(context, prototype, "createTextNode", documentCreateTextNode, 1);
    defineMethod(context, prototype, "createComment", documentCreateComment, 1);
    defineMethod(context, prototype, "createDocumentFragment", documentCreateDocumentFragment, 0);
    defineMethod(context, prototype, "write", documentWrite, 1);
    defineMethod(context, prototype, "writeln", documentWrite, 1);

    defineGetter(context, prototype, "documentElement", documentGetDocumentElement);
    defineGetter(context, prototype, "head", documentGetHead);
    defineGetter(context, prototype, "body", documentGetBody);
    defineAccessor(context, prototype, "title", documentGetTitle, documentSetTitle);
    defineGetter(context, prototype, "URL", documentGetUrl);
    defineGetter(context, prototype, "readyState", documentGetReadyState);

    // The interface-name members a browser exposes on document.
    defineString(context, prototype, "compatMode", QStringLiteral("BackCompat"));
}

} // namespace oqb::javascript::detail

#endif // OPENQBROWSER_SCRIPTING
