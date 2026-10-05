#include "dom/Document.h"

#include <functional>

namespace oqb::dom {

Document::Document(const oqb::network::Url &url)
    : Node(NodeType::Document)
    , m_documentUrl(url)
{
}

Document::~Document() = default;

std::unique_ptr<Document> Document::create(const oqb::network::Url &url)
{
    return std::make_unique<Document>(url);
}

void Document::setDocumentElement(std::unique_ptr<Element> element)
{
    m_documentElement = nullptr;
    // Replace whatever root was there before, if any.
    clearChildren();
    if (element)
        m_documentElement = static_cast<Element *>(appendChild(std::move(element)));
}

Element *Document::head() const
{
    if (!m_documentElement)
        return nullptr;
    for (Element *child : m_documentElement->childElements()) {
        if (child->isTag(QStringLiteral("head")))
            return child;
    }
    return nullptr;
}

Element *Document::body() const
{
    if (!m_documentElement)
        return nullptr;
    for (Element *child : m_documentElement->childElements()) {
        if (child->isTag(QStringLiteral("body")))
            return child;
    }
    // A frameset document has no body; fall back to the root so layout still
    // has something to measure.
    return nullptr;
}

std::unique_ptr<Element> Document::createElement(const QString &tagName)
{
    return std::make_unique<Element>(tagName);
}

std::unique_ptr<Text> Document::createTextNode(const QString &data)
{
    return std::make_unique<Text>(data);
}

std::unique_ptr<Comment> Document::createComment(const QString &data)
{
    return std::make_unique<Comment>(data);
}

Element *Document::getElementById(const QString &id) const
{
    if (id.isEmpty() || !m_documentElement)
        return nullptr;
    for (Element *element : m_documentElement->inclusiveDescendantElements()) {
        if (element->id() == id)
            return element;
    }
    return nullptr;
}

QList<Element *> Document::getElementsByTagName(const QString &tagName) const
{
    QList<Element *> out;
    if (!m_documentElement)
        return out;
    for (Element *element : m_documentElement->inclusiveDescendantElements()) {
        if (element->isTag(tagName))
            out.append(element);
    }
    return out;
}

QList<Element *> Document::getElementsByClass(const QString &className) const
{
    QList<Element *> out;
    if (!m_documentElement)
        return out;
    for (Element *element : m_documentElement->inclusiveDescendantElements()) {
        if (element->classList().contains(className))
            out.append(element);
    }
    return out;
}

QString Document::title() const
{
    const QList<Element *> titles = getElementsByTagName(QStringLiteral("title"));
    if (!titles.isEmpty()) {
        const QString text = titles.first()->textContent().simplified();
        if (!text.isEmpty())
            return text;
    }
    // Browsers fall back to the first heading when no <title> was provided.
    for (const QString &heading : {QStringLiteral("h1"), QStringLiteral("h2"), QStringLiteral("h3")}) {
        const QList<Element *> elements = getElementsByTagName(heading);
        if (!elements.isEmpty()) {
            const QString text = elements.first()->textContent().simplified();
            if (!text.isEmpty())
                return text;
        }
    }
    return {};
}

void Document::setTitle(const QString &title)
{
    Element *headElement = head();
    if (!headElement)
        return;

    QList<Element *> titles = getElementsByTagName(QStringLiteral("title"));
    Element *titleElement = titles.isEmpty() ? nullptr : titles.first();

    if (!titleElement) {
        auto created = createElement(QStringLiteral("title"));
        titleElement = created.release();
        headElement->appendChild(std::unique_ptr<Node>(titleElement));
    }

    while (titleElement->firstChild())
        titleElement->clearChildren();
    titleElement->appendChild(createTextNode(title));
}

QString Document::metaDescription() const
{
    for (Element *meta : getElementsByTagName(QStringLiteral("meta"))) {
        if (meta->attribute(QStringLiteral("name")).compare(QLatin1String("description"),
                                                            Qt::CaseInsensitive)
            == 0) {
            return meta->attribute(QStringLiteral("content"));
        }
    }
    return {};
}

oqb::network::Url Document::resolveUrl(const QString &relative) const
{
    const QString trimmed = relative.trimmed();
    if (trimmed.isEmpty())
        return {};

    for (Element *base : getElementsByTagName(QStringLiteral("base"))) {
        const QString href = base->attribute(QStringLiteral("href")).trimmed();
        if (!href.isEmpty())
            return oqb::network::Url::parse(href, m_documentUrl).resolved(trimmed);
    }

    return m_documentUrl.resolved(trimmed);
}

QString Document::textContent() const
{
    QString out;
    for (const auto &child : children())
        out += child->textContent();
    return out;
}

QString Document::toHtml() const
{
    QString out;
    for (const auto &child : children())
        out += child->toHtml();
    return out;
}

QString Document::toTreeString() const
{
    QString out;

    std::function<void(const Node *, int)> walk = [&](const Node *node, int depth) {
        out += QString(depth * 2, u' ');

        if (node->isText()) {
            const auto *text = static_cast<const Text *>(node);
            const QString preview = text->data().simplified().left(60);
            if (!preview.isEmpty())
                out += QStringLiteral("\"") + preview + QStringLiteral("\"\n");
            else
                out += QStringLiteral("#text (whitespace)\n");
        } else if (node->isComment()) {
            out += QStringLiteral("<!-- ")
                + static_cast<const Comment *>(node)->data().simplified().left(60)
                + QStringLiteral(" -->\n");
        } else {
            out += node->nodeName();
            if (node->attributes() && node->attributes()->size() > 0) {
                for (const AttributeMap::Entry &entry : node->attributes()->entries())
                    out += u' ' + entry.first + QStringLiteral("=\"") + entry.second + u'"';
            }
            out += u'\n';
        }

        for (const auto &child : node->children())
            walk(child.get(), depth + 1);
    };

    if (m_documentElement)
        walk(m_documentElement, 0);
    return out;
}

int Document::nodeCount() const
{
    if (!m_documentElement)
        return 0;
    return 1 + static_cast<int>(m_documentElement->descendants().size());
}

} // namespace oqb::dom
