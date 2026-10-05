#pragma once

#include <QString>
#include <QStringList>

#include <memory>

#include "dom/Node.h"
#include "network/Url.h"

namespace oqb::dom {

/// The document node: the root of a parsed page.
///
/// A Document also carries the metadata the rest of the browser needs: the URL
/// it was loaded from, the declared character encoding, and the quirks-mode flag
/// that the CSS cascade consults.
class Document : public Node
{
public:
    explicit Document(const oqb::network::Url &url = {});
    ~Document() override;

    /// Creates a document whose documentElement() is ready to be filled.
    static std::unique_ptr<Document> create(const oqb::network::Url &url = {});

    oqb::network::Url url() const { return m_documentUrl; }
    void setUrl(const oqb::network::Url &url) { m_documentUrl = url; }

    /// The <html> element, or nullptr for an empty document.
    Element *documentElement() const { return m_documentElement; }
    /// Takes ownership of the root element and installs it as the first child.
    void setDocumentElement(std::unique_ptr<Element> element);

    /// <head>, if the tree has one.
    Element *head() const;
    /// <body>, if the tree has one.
    Element *body() const;

    std::unique_ptr<Element> createElement(const QString &tagName);
    std::unique_ptr<Text> createTextNode(const QString &data);
    std::unique_ptr<Comment> createComment(const QString &data);

    /// First element with the given id anywhere in the document.
    Element *getElementById(const QString &id) const;

    /// All elements with the given tag name, ignoring case.
    QList<Element *> getElementsByTagName(const QString &tagName) const;

    /// All elements carrying the given class.
    QList<Element *> getElementsByClass(const QString &className) const;

    /// The document title: <title> if present, otherwise the first h1.
    QString title() const;
    void setTitle(const QString &title);

    /// The <meta name="description"> content, if present.
    QString metaDescription() const;

    /// Resolves `relative` against the document URL, honouring <base href>.
    oqb::network::Url resolveUrl(const QString &relative) const;

    QString encoding() const { return m_encoding; }
    void setEncoding(const QString &encoding) { m_encoding = encoding; }

    /// True when the document was parsed without a standards doctype. Quirks
    /// mode changes box sizing and other cascade rules.
    bool quirksMode() const { return m_quirksMode; }
    void setQuirksMode(bool quirks) { m_quirksMode = quirks; }

    /// Serialises the whole document back to HTML.
    QString toHtml() const override;

    /// A pretty-printed tree for the DevTools inspector.
    QString toTreeString() const;

    /// Number of nodes in the tree, used by tests and the inspector.
    int nodeCount() const;

    QString nodeName() const override { return QStringLiteral("#document"); }
    QString textContent() const override;

private:
    oqb::network::Url m_documentUrl;
    Element *m_documentElement = nullptr;
    QString m_encoding = QStringLiteral("UTF-8");
    bool m_quirksMode = false;
};

} // namespace oqb::dom
