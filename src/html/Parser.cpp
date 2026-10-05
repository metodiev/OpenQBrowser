#include "html/Parser.h"

#include "html/Tokenizer.h"

#include <QList>
#include <QSet>
#include <QStringList>

namespace oqb::html {
namespace {

/// Elements that never have children (§13.1.2).
bool isVoidElement(const QString &tag)
{
    static const QSet<QString> kVoid = {
        QStringLiteral("area"),   QStringLiteral("base"),   QStringLiteral("br"),
        QStringLiteral("col"),    QStringLiteral("embed"),  QStringLiteral("hr"),
        QStringLiteral("img"),    QStringLiteral("input"),  QStringLiteral("link"),
        QStringLiteral("meta"),   QStringLiteral("param"),  QStringLiteral("source"),
        QStringLiteral("track"),  QStringLiteral("wbr"),    QStringLiteral("basefont"),
        QStringLiteral("bgsound"), QStringLiteral("frame"), QStringLiteral("keygen"),
    };
    return kVoid.contains(tag);
}

/// Elements a <p> is implicitly closed by (§13.2.6.4.7).
bool closesParagraph(const QString &tag)
{
    static const QSet<QString> kClosers = {
        QStringLiteral("address"), QStringLiteral("article"),  QStringLiteral("aside"),
        QStringLiteral("blockquote"), QStringLiteral("details"), QStringLiteral("div"),
        QStringLiteral("dl"),      QStringLiteral("fieldset"), QStringLiteral("figcaption"),
        QStringLiteral("figure"),  QStringLiteral("footer"),   QStringLiteral("form"),
        QStringLiteral("h1"),      QStringLiteral("h2"),       QStringLiteral("h3"),
        QStringLiteral("h4"),      QStringLiteral("h5"),       QStringLiteral("h6"),
        QStringLiteral("header"),  QStringLiteral("hgroup"),   QStringLiteral("hr"),
        QStringLiteral("main"),    QStringLiteral("menu"),     QStringLiteral("nav"),
        QStringLiteral("ol"),      QStringLiteral("p"),        QStringLiteral("pre"),
        QStringLiteral("search"),  QStringLiteral("section"),  QStringLiteral("table"),
        QStringLiteral("ul"),
    };
    return kClosers.contains(tag);
}

/// Elements that may only appear inside <table>, so a stray one in body content
/// is dropped rather than inserted (§13.2.6.4.6).
bool isTableOnly(const QString &tag)
{
    static const QSet<QString> kTableOnly = {
        QStringLiteral("caption"), QStringLiteral("col"),     QStringLiteral("colgroup"),
        QStringLiteral("tbody"),   QStringLiteral("td"),      QStringLiteral("tfoot"),
        QStringLiteral("th"),      QStringLiteral("thead"),   QStringLiteral("tr"),
    };
    return kTableOnly.contains(tag);
}

/// Content model of <head> (§13.2.6.4.4).
bool isHeadElement(const QString &tag)
{
    static const QSet<QString> kHead = {
        QStringLiteral("base"),     QStringLiteral("basefont"), QStringLiteral("bgsound"),
        QStringLiteral("link"),     QStringLiteral("meta"),     QStringLiteral("title"),
        QStringLiteral("noscript"), QStringLiteral("noframes"), QStringLiteral("style"),
        QStringLiteral("template"), QStringLiteral("script"),
    };
    return kHead.contains(tag);
}

/// Tags that are transparent for formatting and inherit from their parent.
bool isWhitespace(const QString &text)
{
    for (const QChar c : text) {
        if (!c.isSpace())
            return false;
    }
    return true;
}

} // namespace

namespace {

/// The tree builder.
///
/// The standard describes this as a stack of open elements plus a current
/// insertion mode. OpenQBrowser implements the same two structures, with the
/// insertion modes condensed into the cases that matter for static documents.
class TreeBuilder
{
public:
    TreeBuilder(dom::Document *document, const ParseOptions &options)
        : m_document(document)
        , m_options(options)
    {
    }

    QStringList warnings() const { return m_warnings; }

    void run(const QString &html)
    {
        Tokenizer tokenizer(html);

        // Start in "before html": the root element is created on demand.
        m_tokenizer = &tokenizer;
        while (true) {
            Token token = tokenizer.nextToken();
            if (token.type == Token::Type::EndOfFile)
                break;
            process(token);
        }
        finishPendingText();

        // §13.2.6.4.1: a document with no doctype at all is in quirks mode.
        // Only a well-formed "<!DOCTYPE html>" leaves it in standards mode.
        if (!m_sawDoctype) {
            m_document->setQuirksMode(true);
            m_warnings.append(QStringLiteral("Missing doctype; document parsed in quirks mode"));
        }
    }

private:
    // ----------------------------------------------------------- tree helpers

    dom::Element *currentNode() const
    {
        return m_openElements.isEmpty() ? nullptr : m_openElements.last();
    }

    dom::Element *ensureHtmlElement()
    {
        if (!m_html) {
            auto html = m_document->createElement(QStringLiteral("html"));
            // setDocumentElement() also records the root so that Document can
            // find head() and body() later.
            m_document->setDocumentElement(std::move(html));
            m_html = m_document->documentElement();
            m_openElements.append(m_html);
        }
        return m_html;
    }

    dom::Element *ensureHeadElement()
    {
        ensureHtmlElement();
        if (!m_head) {
            auto head = m_document->createElement(QStringLiteral("head"));
            m_head = static_cast<dom::Element *>(m_html->appendChild(std::move(head)));
        }
        return m_head;
    }

    dom::Element *ensureBodyElement()
    {
        ensureHtmlElement();
        if (!m_body) {
            auto body = m_document->createElement(QStringLiteral("body"));
            m_body = static_cast<dom::Element *>(m_html->appendChild(std::move(body)));
            m_openElements.append(m_body);
        }
        return m_body;
    }

    /// True while the parser is building <head>, i.e. before <body> exists and
    /// no body content has been seen.
    bool inHead() const { return m_body == nullptr && !m_bodyStarted; }

    void appendToCurrent(dom::Node *node)
    {
        if (!node)
            return;
        dom::Element *parent = currentNode();
        if (!parent) {
            parent = ensureBodyElement();
        }
        // A pointer-to-raw is re-wrapped: the tree owns the node from here.
        std::unique_ptr<dom::Node> owned(node);
        parent->appendChild(std::move(owned));
    }

    using OwnedNode = std::unique_ptr<dom::Node>;
    OwnedNode detach(dom::Node *node)
    {
        if (!node || !node->parent())
            return OwnedNode(node);
        return node->parent()->removeChild(node);
    }

    // --------------------------------------------------------------- handling

    void process(const Token &token)
    {
        switch (token.type) {
        case Token::Type::Character:
            processCharacters(token.data);
            break;
        case Token::Type::Comment:
            processComment(token.data);
            break;
        case Token::Type::Doctype:
            processDoctype(token);
            break;
        case Token::Type::StartTag:
            processStartTag(token);
            break;
        case Token::Type::EndTag:
            processEndTag(token.name);
            break;
        case Token::Type::EndOfFile:
            break;
        }
    }

    void processDoctype(const Token &token)
    {
        m_sawDoctype = true;
        m_document->setQuirksMode(token.forceQuirks
                                  || token.name != QLatin1String("html"));
    }

    void processComment(const QString &data)
    {
        flushText();
        m_document->appendChild(m_document->createComment(data));
    }

    void processCharacters(const QString &data)
    {
        // An empty character token carries no content; ignoring it keeps the
        // parser from treating it as buffered text.
        if (data.isEmpty())
            return;
        // Adjacent character tokens are buffered so they become one text node.
        m_pendingText += data;
    }

    void flushText()
    {
        if (m_pendingText.isEmpty())
            return;

        const QString text = m_pendingText;
        m_pendingText.clear();

        // Text is only "in the head slot" when it is being inserted directly
        // into <html> or <head>. Text inside a head child such as <title> or
        // <style> belongs to that element and must not start the body.
        const bool headSlot = currentNode() == nullptr || currentNode() == m_html
            || currentNode() == m_head;

        if (headSlot) {
            if (m_body == nullptr) {
                if (isWhitespace(text))
                    return; // Insignificant whitespace inside <head>.
                // Non-whitespace in the head slot starts the body instead
                // (§13.2.6.4.4).
                startBody();
            } else if (isWhitespace(text)) {
                return; // Whitespace between </head> and <body>.
            }
        }

        appendToCurrent(m_document->createTextNode(text).release());
    }

    void finishPendingText()
    {
        flushText();
    }

    void processStartTag(const Token &token)
    {
        const QString &tag = token.name;

        // ------------------------------------------------ head-only elements
        if (isHeadElement(tag) && !m_bodyStarted) {
            flushText();
            ensureHeadElement();

            std::unique_ptr<dom::Element> element = m_document->createElement(tag);
            dom::Element *created = element.get();
            for (const TokenAttribute &attribute : token.attributes)
                created->attr().append(attribute.name, attribute.value);

            if (isVoidElement(tag) || token.selfClosing) {
                m_head->appendChild(std::move(element));
                return;
            }

            m_head->appendChild(std::move(element));
            // <title>, <style> and <script> carry text content, which their end
            // tag closes; keeping them open lets the tokenizer's raw text mode
            // deliver the content into the right element.
            m_openElements.append(created);
            return;
        }

        if (tag == QLatin1String("html")) {
            flushText();
            if (m_html) {
                // A second <html> only contributes its attributes.
                for (const TokenAttribute &attribute : token.attributes) {
                    if (!m_html->hasAttribute(attribute.name))
                        m_html->attr().append(attribute.name, attribute.value);
                }
                return;
            }
            ensureHtmlElement();
            for (const TokenAttribute &attribute : token.attributes)
                m_html->attr().append(attribute.name, attribute.value);
            return;
        }

        if (tag == QLatin1String("head")) {
            flushText();
            ensureHeadElement();
            if (m_bodyStarted)
                return; // A late <head> is ignored.
            if (openElementIndex(QStringLiteral("head")) < 0)
                m_openElements.append(m_head);
            return;
        }

        if (tag == QLatin1String("body")) {
            flushText();
            startBody();
            for (const TokenAttribute &attribute : token.attributes) {
                if (!m_body->hasAttribute(attribute.name))
                    m_body->attr().append(attribute.name, attribute.value);
            }
            return;
        }

        // ------------------------------------------------------------ content
        if (inHead())
            startBody();

        // Auto-close an open <p> when a block element starts.
        if (closesParagraph(tag))
            closeOpenElement(QStringLiteral("p"));

        // <li> closes an open <li>; <dd>/<dt> close each other as well.
        if (tag == QLatin1String("li"))
            closeOpenElement(QStringLiteral("li"));
        if (tag == QLatin1String("dd") || tag == QLatin1String("dt")) {
            closeOpenElement(QStringLiteral("dd"));
            closeOpenElement(QStringLiteral("dt"));
        }

        // Table sections close each other, and a new cell closes the last one.
        if (tag == QLatin1String("tr") || tag == QLatin1String("tbody")
            || tag == QLatin1String("thead") || tag == QLatin1String("tfoot")) {
            closeOpenElement(QStringLiteral("td"));
            closeOpenElement(QStringLiteral("th"));
        }
        if (tag == QLatin1String("td") || tag == QLatin1String("th")) {
            closeOpenElement(QStringLiteral("td"));
            closeOpenElement(QStringLiteral("th"));
        }

        // A heading closes any open heading (§13.2.6.4.7).
        if (tag.size() == 2 && tag.at(0) == u'h' && tag.at(1) >= u'1' && tag.at(1) <= u'6') {
            for (const QString &heading : {QStringLiteral("h1"), QStringLiteral("h2"),
                                           QStringLiteral("h3"), QStringLiteral("h4"),
                                           QStringLiteral("h5"), QStringLiteral("h6")}) {
                closeOpenElement(heading);
            }
        }

        // Stray table-only elements outside a table are ignored.
        if (isTableOnly(tag) && currentNode()
            && !currentNode()->isTag(QStringLiteral("table"))
            && !currentNode()->isTag(QStringLiteral("tbody"))
            && !currentNode()->isTag(QStringLiteral("thead"))
            && !currentNode()->isTag(QStringLiteral("tfoot"))
            && !currentNode()->isTag(QStringLiteral("tr"))
            && !currentNode()->isTag(QStringLiteral("colgroup"))) {
            m_warnings.append(QStringLiteral("Ignored <%1> outside a table").arg(tag));
            return;
        }

        flushText();

        if (m_openElements.size() > m_options.maxDepth) {
            m_warnings.append(QStringLiteral("Maximum element depth reached at <%1>").arg(tag));
            return;
        }

        auto element = m_document->createElement(tag);
        for (const TokenAttribute &attribute : token.attributes)
            element->attr().append(attribute.name, attribute.value);

        dom::Element *created = element.get();
        appendToCurrent(element.release());

        // Void elements and self-closing tags are never pushed as open
        // elements. The HTML parser ignores the "/" on non-void elements, which
        // matches browsers, but for foreign content it is honoured.
        if (isVoidElement(tag) || token.selfClosing)
            return;

        m_openElements.append(created);
    }

    void processEndTag(const QString &tag)
    {
        if (tag.isEmpty())
            return;

        if (tag == QLatin1String("head")) {
            flushText();
            closeOpenElement(QStringLiteral("head"));
            return;
        }

        if (tag == QLatin1String("body") || tag == QLatin1String("html")) {
            flushText();
            m_bodyClosed = true;
            return;
        }

        flushText();

        // Find the nearest matching open element and close through it. Anything
        // left open between it and the top is closed implicitly, which is the
        // behaviour that makes real pages work despite their markup errors.
        int index = -1;
        for (int i = static_cast<int>(m_openElements.size()) - 1; i >= 0; --i) {
            if (m_openElements.at(i)->isTag(tag)) {
                index = i;
                break;
            }
        }

        if (index < 0) {
            if (isVoidElement(tag) || isKnownElement(tag))
                m_warnings.append(QStringLiteral("Stray </%1> tag ignored").arg(tag));
            else
                m_warnings.append(QStringLiteral("Unknown end tag </%1> ignored").arg(tag));
            return;
        }

        while (m_openElements.size() > index)
            m_openElements.removeLast();
    }

    void closeOpenElement(const QString &tag)
    {
        const int index = openElementIndex(tag);
        if (index < 0)
            return;
        flushText();
        while (m_openElements.size() > index)
            m_openElements.removeLast();
    }

    int openElementIndex(const QString &tag) const
    {
        for (int i = static_cast<int>(m_openElements.size()) - 1; i >= 0; --i) {
            if (m_openElements.at(i)->isTag(tag))
                return i;
        }
        return -1;
    }

    static bool isKnownElement(const QString &tag)
    {
        static const QSet<QString> kKnown = {
            QStringLiteral("a"),      QStringLiteral("abbr"),   QStringLiteral("address"),
            QStringLiteral("area"),   QStringLiteral("article"), QStringLiteral("aside"),
            QStringLiteral("audio"),  QStringLiteral("b"),      QStringLiteral("base"),
            QStringLiteral("bdi"),    QStringLiteral("bdo"),    QStringLiteral("blockquote"),
            QStringLiteral("body"),   QStringLiteral("br"),     QStringLiteral("button"),
            QStringLiteral("canvas"), QStringLiteral("caption"), QStringLiteral("cite"),
            QStringLiteral("code"),   QStringLiteral("col"),    QStringLiteral("colgroup"),
            QStringLiteral("data"),   QStringLiteral("datalist"), QStringLiteral("dd"),
            QStringLiteral("del"),    QStringLiteral("details"), QStringLiteral("dfn"),
            QStringLiteral("dialog"), QStringLiteral("div"),    QStringLiteral("dl"),
            QStringLiteral("dt"),     QStringLiteral("em"),     QStringLiteral("embed"),
            QStringLiteral("fieldset"), QStringLiteral("figcaption"), QStringLiteral("figure"),
            QStringLiteral("footer"), QStringLiteral("form"),   QStringLiteral("h1"),
            QStringLiteral("h2"),     QStringLiteral("h3"),     QStringLiteral("h4"),
            QStringLiteral("h5"),     QStringLiteral("h6"),     QStringLiteral("head"),
            QStringLiteral("header"), QStringLiteral("hgroup"), QStringLiteral("hr"),
            QStringLiteral("html"),   QStringLiteral("i"),      QStringLiteral("iframe"),
            QStringLiteral("img"),    QStringLiteral("input"),  QStringLiteral("ins"),
            QStringLiteral("kbd"),    QStringLiteral("label"),  QStringLiteral("legend"),
            QStringLiteral("li"),     QStringLiteral("link"),   QStringLiteral("main"),
            QStringLiteral("map"),    QStringLiteral("mark"),   QStringLiteral("menu"),
            QStringLiteral("meta"),   QStringLiteral("meter"),  QStringLiteral("nav"),
            QStringLiteral("noscript"), QStringLiteral("object"), QStringLiteral("ol"),
            QStringLiteral("optgroup"), QStringLiteral("option"), QStringLiteral("output"),
            QStringLiteral("p"),      QStringLiteral("picture"), QStringLiteral("pre"),
            QStringLiteral("progress"), QStringLiteral("q"),    QStringLiteral("rp"),
            QStringLiteral("rt"),     QStringLiteral("ruby"),   QStringLiteral("s"),
            QStringLiteral("samp"),   QStringLiteral("script"), QStringLiteral("section"),
            QStringLiteral("select"), QStringLiteral("slot"),   QStringLiteral("small"),
            QStringLiteral("source"), QStringLiteral("span"),   QStringLiteral("strong"),
            QStringLiteral("style"),  QStringLiteral("sub"),    QStringLiteral("summary"),
            QStringLiteral("sup"),    QStringLiteral("table"),  QStringLiteral("tbody"),
            QStringLiteral("td"),     QStringLiteral("template"), QStringLiteral("textarea"),
            QStringLiteral("tfoot"),  QStringLiteral("th"),     QStringLiteral("thead"),
            QStringLiteral("time"),   QStringLiteral("title"),  QStringLiteral("tr"),
            QStringLiteral("track"),  QStringLiteral("u"),      QStringLiteral("ul"),
            QStringLiteral("var"),    QStringLiteral("video"),  QStringLiteral("wbr"),
        };
        return kKnown.contains(tag);
    }

    void startBody()
    {
        flushText();
        // Everything that was open belongs to <head>; drop back to <html>.
        while (m_openElements.size() > 1)
            m_openElements.removeLast();
        m_bodyStarted = true;
        ensureBodyElement();
    }

    dom::Document *m_document;
    ParseOptions m_options;
    Tokenizer *m_tokenizer = nullptr;

    dom::Element *m_html = nullptr;
    dom::Element *m_head = nullptr;
    dom::Element *m_body = nullptr;

    QList<dom::Element *> m_openElements;
    QString m_pendingText;
    QStringList m_warnings;
    bool m_bodyStarted = false;
    bool m_bodyClosed = false;
    bool m_sawDoctype = false;
};

} // namespace

ParseResult Parser::parse(const QString &html, const oqb::network::Url &url,
                          const ParseOptions &options)
{
    ParseResult result;
    result.document = dom::Document::create(url);

    TreeBuilder builder(result.document.get(), options);
    builder.run(html);
    result.warnings = builder.warnings();

    // §13.2.6.4.1: a document always ends up with html, head and body. The
    // tree builder creates them on demand, but a document with no markup at all
    // or with content only in the body still needs the full skeleton, because
    // everything downstream looks head() and body() up.
    if (!result.document->documentElement()) {
        auto documentElement = result.document->createElement(QStringLiteral("html"));
        result.document->setDocumentElement(std::move(documentElement));
    }

    dom::Element *root = result.document->documentElement();
    if (root && !result.document->head()) {
        auto head = result.document->createElement(QStringLiteral("head"));
        // The head comes first, before any body content that already exists.
        if (dom::Element *body = result.document->body())
            root->insertBefore(std::move(head), body);
        else
            root->appendChild(std::move(head));
    }

    if (root && !result.document->body()) {
        auto body = result.document->createElement(QStringLiteral("body"));
        root->appendChild(std::move(body));
    }

    return result;
}

QList<dom::Node *> Parser::parseFragment(const QString &html, dom::Document *document,
                                         const QString &contextTag)
{
    QList<dom::Node *> nodes;
    if (!document)
        return nodes;

    ParseResult result = parse(html, {}, {});
    if (!result.document)
        return nodes;

    // Where the parser would insert depends on the context element; the common
    // cases are covered explicitly and everything else falls back to <body>.
    dom::Element *container = nullptr;
    if (contextTag.compare(QLatin1String("tr"), Qt::CaseInsensitive) == 0
        || contextTag.compare(QLatin1String("tbody"), Qt::CaseInsensitive) == 0
        || contextTag.compare(QLatin1String("thead"), Qt::CaseInsensitive) == 0
        || contextTag.compare(QLatin1String("tfoot"), Qt::CaseInsensitive) == 0
        || contextTag.compare(QLatin1String("table"), Qt::CaseInsensitive) == 0) {
        const QList<dom::Element *> tables
            = result.document->getElementsByTagName(QStringLiteral("table"));
        if (!tables.isEmpty())
            container = tables.first();
    } else if (contextTag.compare(QLatin1String("head"), Qt::CaseInsensitive) == 0) {
        container = result.document->head();
    }

    if (!container)
        container = result.document->body();
    if (!container)
        container = result.document->documentElement();

    if (container) {
        qsizetype index = 0;
        while (index < static_cast<qsizetype>(container->children().size())) {
            auto child = container->removeChild(container->childAt(static_cast<int>(index)));
            if (!child)
                break;
            nodes.append(child.release());
        }
    }

    return nodes;
}

} // namespace oqb::html
