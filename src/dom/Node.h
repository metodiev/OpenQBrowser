#pragma once

#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace oqb::dom {

class Document;
class Element;
class Node;

/// DOM node types, matching the values used by the DOM standard.
enum class NodeType {
    Element = 1,
    Text = 3,
    Comment = 8,
    Document = 9,
    DocumentType = 10,
};

/// Attribute storage for an element. Names are stored lower-case, as the HTML
/// parser produces them, while values keep their original casing.
class AttributeMap
{
public:
    using Entry = QPair<QString, QString>;

    void set(const QString &name, const QString &value);
    void append(const QString &name, const QString &value);
    bool remove(const QString &name);

    bool contains(const QString &name) const;
    QString value(const QString &name, const QString &defaultValue = {}) const;
    bool hasValue(const QString &name) const { return contains(name); }

    QString id() const { return value(QStringLiteral("id")); }
    QStringList classList() const;

    const QList<Entry> &entries() const { return m_entries; }
    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }

private:
    QList<Entry> m_entries;
};

/// Base class of everything in the tree: elements, text nodes, comments.
class Node
{
public:
    explicit Node(NodeType type);
    virtual ~Node();

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    NodeType type() const { return m_type; }
    bool isElement() const { return m_type == NodeType::Element; }
    bool isText() const { return m_type == NodeType::Text; }
    bool isComment() const { return m_type == NodeType::Comment; }
    bool isDocument() const { return m_type == NodeType::Document; }

    Node *parent() const { return m_parent; }
    /// Direct children in document order. std::vector rather than QList because
    /// the children are owned by unique_ptr, which is move-only.
    const std::vector<std::unique_ptr<Node>> &children() const { return m_children; }

    /// Appends an existing child, detaching it from any previous parent.
    Node *appendChild(std::unique_ptr<Node> child);
    /// Inserts before `reference`; appends when `reference` is null or unknown.
    Node *insertBefore(std::unique_ptr<Node> child, Node *reference);
    /// Moves `node` to a new position among this node's children.
    void moveChildTo(Node *node, int index);
    std::unique_ptr<Node> removeChild(Node *child);
    /// Removes and destroys every child.
    void clearChildren();

    int childCount() const { return static_cast<int>(m_children.size()); }
    Node *childAt(int index) const;
    Node *firstChild() const { return childAt(0); }
    Node *lastChild() const { return childAt(childCount() - 1); }
    int indexInParent() const;

    Node *nextSibling() const;
    Node *previousSibling() const;

    /// Depth-first search for the first descendant satisfying `predicate`.
    template <typename Predicate>
    Node *findFirst(Predicate predicate) const
    {
        for (const auto &child : m_children) {
            if (predicate(child.get()))
                return child.get();
            if (Node *found = child->findFirst(predicate))
                return found;
        }
        return nullptr;
    }

    /// Every descendant in document order.
    QList<Node *> descendants() const;

    Element *parentElement() const;
    Document *ownerDocument() const;

    /// Nearest ancestor element (or this node) matching `id`.
    Element *closestById(const QString &id) const;

    virtual QString nodeName() const = 0;

    /// The node's attributes, or nullptr for non-elements. Allows callers to
    /// avoid qobject_cast-style downcasts when walking the tree.
    virtual const AttributeMap *attributes() const { return nullptr; }
    virtual AttributeMap *attributes() { return nullptr; }

    /// Text of this node and its descendants, as rendered by a browser.
    virtual QString textContent() const = 0;

    /// Serialises the subtree as HTML. The result round-trips through the
    /// parser closely enough for inspection in the DevTools panel.
    virtual QString toHtml() const = 0;

    /// A short human readable label used by the inspector, e.g. "div#main".
    QString describe() const;

protected:
    NodeType m_type;
    Node *m_parent = nullptr;
    std::vector<std::unique_ptr<Node>> m_children;
};

/// An element node such as <p> or <img>.
class Element : public Node
{
public:
    explicit Element(const QString &tagName);

    QString tagName() const { return m_tagName; }
    QString localName() const { return m_tagName; }

    /// Namespace-agnostic check: "DIV" and "div" both match "div".
    bool isTag(const QString &name) const
    {
        return m_tagName.compare(name, Qt::CaseInsensitive) == 0;
    }

    const AttributeMap *attributes() const override { return &m_attributes; }
    AttributeMap *attributes() override { return &m_attributes; }
    AttributeMap &attr() { return m_attributes; }
    const AttributeMap &attr() const { return m_attributes; }

    QString id() const { return m_attributes.id(); }
    QString className() const { return m_attributes.value(QStringLiteral("class")); }
    QStringList classList() const { return m_attributes.classList(); }

    QString attribute(const QString &name, const QString &defaultValue = {}) const
    {
        return m_attributes.value(name, defaultValue);
    }
    bool hasAttribute(const QString &name) const { return m_attributes.contains(name); }

    QString nodeName() const override;
    QString textContent() const override;
    QString toHtml() const override;

    /// The children that are elements, in document order.
    QList<Element *> childElements() const;

    /// First element child, or nullptr.
    Element *firstElementChild() const;
    Element *lastElementChild() const;

    /// Next sibling that is an element, or nullptr.
    Element *nextElementSibling() const;
    Element *previousElementSibling() const;

    /// All descendant elements in document order (excluding this element).
    QList<Element *> descendantElements() const;

    /// This element and all descendants, for selector matching.
    QList<Element *> inclusiveDescendantElements() const;

private:
    QString m_tagName;
    AttributeMap m_attributes;
};

/// A text node.
class Text : public Node
{
public:
    explicit Text(const QString &data);

    const QString &data() const { return m_data; }
    void setData(const QString &data) { m_data = data; }

    /// Collapses runs of ASCII whitespace the way CSS does by default, and
    /// trims the ends. Used by the layout engine.
    QString collapsedText() const;
    bool isWhitespaceOnly() const;

    QString nodeName() const override;
    QString textContent() const override { return m_data; }
    QString toHtml() const override;

private:
    QString m_data;
};

/// A comment node. Comments are kept in the tree because the inspector shows
/// them, but they never take part in layout.
class Comment : public Node
{
public:
    explicit Comment(const QString &data);

    const QString &data() const { return m_data; }

    QString nodeName() const override;
    QString textContent() const override { return m_data; }
    QString toHtml() const override;

private:
    QString m_data;
};

} // namespace oqb::dom
