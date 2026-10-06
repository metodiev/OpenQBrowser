#include "dom/Node.h"

#include "dom/Document.h"

#include <QRegularExpression>

namespace oqb::dom {
namespace {

/// Attribute names that must never be treated as boolean attributes.
bool isBooleanAttribute(const QString &name)
{
    static const QStringList names = {
        QStringLiteral("checked"),     QStringLiteral("disabled"), QStringLiteral("hidden"),
        QStringLiteral("selected"),    QStringLiteral("readonly"), QStringLiteral("required"),
        QStringLiteral("multiple"),    QStringLiteral("autofocus"), QStringLiteral("autoplay"),
        QStringLiteral("controls"),    QStringLiteral("loop"),     QStringLiteral("muted"),
        QStringLiteral("open"),        QStringLiteral("novalidate"),
    };
    return names.contains(name);
}

QString escapeText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c == u'&')
            out += QStringLiteral("&amp;");
        else if (c == u'<')
            out += QStringLiteral("&lt;");
        else if (c == u'>')
            out += QStringLiteral("&gt;");
        else
            out += c;
    }
    return out;
}

QString escapeAttribute(const QString &value)
{
    QString out = escapeText(value);
    out.replace(QLatin1String("\""), QLatin1String("&quot;"));
    return out;
}

} // namespace

// ---------------------------------------------------------------- AttributeMap

void AttributeMap::set(const QString &name, const QString &value)
{
    for (Entry &entry : m_entries) {
        if (entry.first == name) {
            entry.second = value;
            return;
        }
    }
    m_entries.append({name, value});
}

void AttributeMap::append(const QString &name, const QString &value)
{
    m_entries.append({name, value});
}

bool AttributeMap::remove(const QString &name)
{
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).first == name) {
            m_entries.removeAt(i);
            return true;
        }
    }
    return false;
}

bool AttributeMap::contains(const QString &name) const
{
    for (const Entry &entry : m_entries) {
        if (entry.first == name)
            return true;
    }
    return false;
}

QString AttributeMap::value(const QString &name, const QString &defaultValue) const
{
    for (const Entry &entry : m_entries) {
        if (entry.first == name)
            return entry.second;
    }
    return defaultValue;
}

QStringList AttributeMap::classList() const
{
    const QString classes = value(QStringLiteral("class"));
    if (classes.isEmpty())
        return {};

    // A hand-written split rather than QString::split with a regular expression.
    // This is on the hottest path in the browser: it runs for every class
    // selector against every element, and building a QRegularExpression each
    // time costs more than a page's entire layout. Class names are separated by
    // ASCII whitespace, so a scan is both faster and exact.
    QStringList result;
    QString current;
    current.reserve(16);

    const auto flush = [&result, &current] {
        if (!current.isEmpty()) {
            result.append(current);
            current.clear();
        }
    };

    for (const QChar c : classes) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f') {
            flush();
            continue;
        }
        current += c;
    }
    flush();

    return result;
}

// ----------------------------------------------------------------------- Node

Node::Node(NodeType type)
    : m_type(type)
{
}

Node::~Node() = default;

Node *Node::appendChild(std::unique_ptr<Node> child)
{
    if (!child)
        return nullptr;

    // A node that already has a parent is moved rather than copied, so the old
    // parent has to hand ownership back. The result of removeChild is adopted
    // instead of the argument, because discarding it would delete the node that
    // is about to be re-inserted.
    if (child->m_parent)
        child = child->m_parent->removeChild(child.get());

    if (!child)
        return nullptr;

    child->m_parent = this;
    if (!child->m_ownerDocument && this->isDocument())
        child->setOwnerDocument(static_cast<Document *>(this));
    Node *raw = child.get();
    m_children.push_back(std::move(child));
    return raw;
}

Node *Node::insertBefore(std::unique_ptr<Node> child, Node *reference)
{
    if (!child)
        return nullptr;
    if (!reference || reference->m_parent != this)
        return appendChild(std::move(child));

    // As in appendChild, a node with an existing parent is moved, and the old
    // parent's ownership has to be adopted rather than dropped.
    if (child->m_parent)
        child = child->m_parent->removeChild(child.get());

    if (!child)
        return nullptr;

    const int index = reference->indexInParent();
    child->m_parent = this;
    if (!child->m_ownerDocument && this->isDocument())
        child->setOwnerDocument(static_cast<Document *>(this));
    Node *raw = child.get();
    m_children.insert(m_children.begin() + index, std::move(child));
    return raw;
}

void Node::moveChildTo(Node *node, int index)
{
    if (!node || node->m_parent != this)
        return;
    const int from = node->indexInParent();
    if (from < 0 || from == index)
        return;

    std::unique_ptr<Node> owned = std::move(m_children[static_cast<size_t>(from)]);
    m_children.erase(m_children.begin() + from);

    int target = qBound(0, index, static_cast<int>(m_children.size()));
    if (from < index)
        --target;
    target = qBound(0, target, static_cast<int>(m_children.size()));

    m_children.insert(m_children.begin() + target, std::move(owned));
}

std::unique_ptr<Node> Node::removeChild(Node *child)
{
    if (!child || child->m_parent != this)
        return nullptr;
    const int index = child->indexInParent();
    if (index < 0)
        return nullptr;
    std::unique_ptr<Node> owned = std::move(m_children[static_cast<size_t>(index)]);
    m_children.erase(m_children.begin() + index);
    owned->m_parent = nullptr;
    return owned;
}

void Node::clearChildren()
{
    for (auto &child : m_children)
        child->m_parent = nullptr;
    m_children.clear();
}

Node *Node::childAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_children.size()))
        return nullptr;
    return m_children[static_cast<size_t>(index)].get();
}

int Node::indexInParent() const
{
    if (!m_parent)
        return -1;
    const auto &siblings = m_parent->m_children;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == this)
            return static_cast<int>(i);
    }
    return -1;
}

Node *Node::nextSibling() const
{
    if (!m_parent)
        return nullptr;
    return m_parent->childAt(indexInParent() + 1);
}

Node *Node::previousSibling() const
{
    const int index = indexInParent();
    if (!m_parent || index <= 0)
        return nullptr;
    return m_parent->childAt(index - 1);
}

QList<Node *> Node::descendants() const
{
    QList<Node *> out;
    for (const auto &child : m_children) {
        out.append(child.get());
        out.append(child->descendants());
    }
    return out;
}

Element *Node::parentElement() const
{
    for (Node *node = m_parent; node; node = node->m_parent) {
        if (node->isElement())
            return static_cast<Element *>(node);
    }
    return nullptr;
}

Document *Node::ownerDocument() const
{
    // A node remembers the document it was created by. Walking up the parents
    // alone is not enough: a detached node has no parent, yet script can still
    // ask it to create children or read its text, and a browser answers.
    if (m_ownerDocument)
        return m_ownerDocument;

    for (Node *node = const_cast<Node *>(this); node; node = node->m_parent) {
        if (node->isDocument())
            return static_cast<Document *>(node);
    }
    return nullptr;
}

Element *Node::closestById(const QString &id) const
{
    for (const Node *node = this; node; node = node->m_parent) {
        if (node->isElement() && node->attributes()->id() == id)
            return const_cast<Element *>(static_cast<const Element *>(node));
    }
    return nullptr;
}

QString Node::describe() const
{
    if (!isElement())
        return nodeName();
    const auto *element = static_cast<const Element *>(this);
    QString label = element->tagName();
    if (!element->id().isEmpty())
        label += u'#' + element->id();
    else if (!element->className().isEmpty())
        label += u'.' + element->classList().join(u'.');
    return label;
}

// -------------------------------------------------------------------- Element

Element::Element(const QString &tagName)
    : Node(NodeType::Element)
    , m_tagName(tagName.toLower())
{
}

QString Element::nodeName() const
{
    // HTML element names are upper-cased in the DOM API but lower-cased on the
    // wire; OpenQBrowser reports them lower-cased for readability.
    return m_tagName;
}

QString Element::textContent() const
{
    QString out;
    for (const auto &child : m_children)
        out += child->textContent();
    return out;
}

QString Element::toHtml() const
{
    static const QStringList voidElements = {
        QStringLiteral("area"),   QStringLiteral("base"),   QStringLiteral("br"),
        QStringLiteral("col"),    QStringLiteral("embed"),  QStringLiteral("hr"),
        QStringLiteral("img"),    QStringLiteral("input"),  QStringLiteral("link"),
        QStringLiteral("meta"),   QStringLiteral("param"),  QStringLiteral("source"),
        QStringLiteral("track"),  QStringLiteral("wbr"),
    };

    // <pre>/<textarea> content is not escaped, matching HTML serialisation.
    const bool rawText = isTag(QStringLiteral("script")) || isTag(QStringLiteral("style"));

    QString out = u'<' + m_tagName;
    for (const AttributeMap::Entry &entry : m_attributes.entries()) {
        out += u' ' + entry.first;
        if (isBooleanAttribute(entry.first) && entry.second.isEmpty())
            continue;
        out += QStringLiteral("=\"") + escapeAttribute(entry.second) + u'"';
    }
    out += u'>';

    if (voidElements.contains(m_tagName))
        return out;

    for (const auto &child : m_children)
        out += rawText ? child->textContent() : child->toHtml();

    out += QStringLiteral("</") + m_tagName + u'>';
    return out;
}

QList<Element *> Element::childElements() const
{
    QList<Element *> out;
    for (const auto &child : m_children) {
        if (child->isElement())
            out.append(static_cast<Element *>(child.get()));
    }
    return out;
}

Element *Element::firstElementChild() const
{
    for (const auto &child : m_children) {
        if (child->isElement())
            return static_cast<Element *>(child.get());
    }
    return nullptr;
}

Element *Element::lastElementChild() const
{
    for (int i = static_cast<int>(m_children.size()) - 1; i >= 0; --i) {
        if (m_children.at(i)->isElement())
            return static_cast<Element *>(m_children.at(i).get());
    }
    return nullptr;
}

Element *Element::nextElementSibling() const
{
    for (Node *node = nextSibling(); node; node = node->nextSibling()) {
        if (node->isElement())
            return static_cast<Element *>(node);
    }
    return nullptr;
}

Element *Element::previousElementSibling() const
{
    for (Node *node = previousSibling(); node; node = node->previousSibling()) {
        if (node->isElement())
            return static_cast<Element *>(node);
    }
    return nullptr;
}

QList<Element *> Element::descendantElements() const
{
    QList<Element *> out;
    for (Node *node : descendants()) {
        if (node->isElement())
            out.append(static_cast<Element *>(node));
    }
    return out;
}

QList<Element *> Element::inclusiveDescendantElements() const
{
    QList<Element *> out;
    out.append(const_cast<Element *>(this));
    out.append(descendantElements());
    return out;
}

// ----------------------------------------------------------------------- Text

Text::Text(const QString &data)
    : Node(NodeType::Text)
    , m_data(data)
{
}

QString Text::nodeName() const
{
    return QStringLiteral("#text");
}

QString Text::toHtml() const
{
    return escapeText(m_data);
}

bool Text::isWhitespaceOnly() const
{
    for (const QChar c : m_data) {
        if (!c.isSpace())
            return false;
    }
    return true;
}

QString Text::collapsedText() const
{
    QString out;
    out.reserve(m_data.size());
    bool inWhitespace = false;
    bool started = false;

    for (const QChar c : m_data) {
        const bool space = c.isSpace();
        if (space) {
            if (started)
                inWhitespace = true;
            continue;
        }
        if (inWhitespace) {
            out += u' ';
            inWhitespace = false;
        }
        out += c;
        started = true;
    }
    return out;
}

// -------------------------------------------------------------------- Comment

Comment::Comment(const QString &data)
    : Node(NodeType::Comment)
    , m_data(data)
{
}

QString Comment::nodeName() const
{
    return QStringLiteral("#comment");
}

QString Comment::toHtml() const
{
    return QStringLiteral("<!--") + m_data + QStringLiteral("-->");
}

} // namespace oqb::dom
