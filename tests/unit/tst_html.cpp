#include <QtTest>

#include "dom/Document.h"
#include "html/Entities.h"
#include "html/Parser.h"
#include "html/Tokenizer.h"

using namespace oqb;

/// Tests for the HTML tokenizer, tree construction and entity decoding.
class HtmlTest : public QObject
{
    Q_OBJECT

private slots:
    void decodesNamedEntities();
    void decodesNumericEntities();
    void leavesUnknownEntitiesAlone();
    void decodesLegacyEntitiesWithoutSemicolon();
    void tokenizesStartAndEndTags();
    void tokenizesAttributes();
    void handlesQuotedAndUnquotedAttributes();
    void keepsFirstDuplicateAttribute();
    void treatsScriptContentAsText();
    void treatsStyleContentAsText();
    void readsComments();
    void buildsElementTree();
    void createsImpliedElements();
    void autoClosesParagraphs();
    void autoClosesListItems();
    void autoClosesTableCells();
    void handlesUnclosedTags();
    void ignoresStrayEndTags();
    void setsQuirksMode();
    void extractsTitle();
    void extractsMetadata();
    void serializesBackToHtml();
    void resolvesUrlsAgainstBase();
};

void HtmlTest::decodesNamedEntities()
{
    QCOMPARE(html::entities::decode(QStringLiteral("a &amp; b")), QStringLiteral("a & b"));
    QCOMPARE(html::entities::decode(QStringLiteral("&lt;div&gt;")), QStringLiteral("<div>"));
    QCOMPARE(html::entities::decode(QStringLiteral("&copy; 2026")), QStringLiteral("\u00A9 2026"));
    QCOMPARE(html::entities::decode(QStringLiteral("&nbsp;")), QString(QChar(0x00A0)));
}

void HtmlTest::decodesNumericEntities()
{
    QCOMPARE(html::entities::decode(QStringLiteral("&#65;&#66;")), QStringLiteral("AB"));
    QCOMPARE(html::entities::decode(QStringLiteral("&#x41;&#x42;")), QStringLiteral("AB"));
    // A code point outside Unicode becomes the replacement character.
    QCOMPARE(html::entities::decode(QStringLiteral("&#x110000;")),
             QString(QChar(0xFFFD)));
    QCOMPARE(html::entities::decode(QStringLiteral("&#0;")), QString(QChar(0xFFFD)));
}

void HtmlTest::leavesUnknownEntitiesAlone()
{
    QCOMPARE(html::entities::decode(QStringLiteral("&notreal;")), QStringLiteral("&notreal;"));
    QCOMPARE(html::entities::decode(QStringLiteral("100% & 50%")), QStringLiteral("100% & 50%"));
    QCOMPARE(html::entities::decode(QStringLiteral("a & b")), QStringLiteral("a & b"));
}

void HtmlTest::decodesLegacyEntitiesWithoutSemicolon()
{
    // These names are valid without a trailing semicolon.
    QCOMPARE(html::entities::decode(QStringLiteral("a &amp b")), QStringLiteral("a & b"));
    QCOMPARE(html::entities::decode(QStringLiteral("2 &lt 3")), QStringLiteral("2 < 3"));
    // But not when followed by an alphanumeric character.
    QCOMPARE(html::entities::decode(QStringLiteral("&ampersand")), QStringLiteral("&ampersand"));
}

void HtmlTest::tokenizesStartAndEndTags()
{
    html::Tokenizer tokenizer(QStringLiteral("<div></div>"));

    const html::Token open = tokenizer.nextToken();
    QCOMPARE(open.type, html::Token::Type::StartTag);
    QCOMPARE(open.name, QStringLiteral("div"));
    QVERIFY(!open.selfClosing);

    const html::Token close = tokenizer.nextToken();
    QCOMPARE(close.type, html::Token::Type::EndTag);
    QCOMPARE(close.name, QStringLiteral("div"));
}

void HtmlTest::tokenizesAttributes()
{
    html::Tokenizer tokenizer(
        QStringLiteral("<a href=\"/x\" title=\"hello\" data-id=42 disabled>"));

    const html::Token token = tokenizer.nextToken();
    QCOMPARE(token.name, QStringLiteral("a"));
    QCOMPARE(token.attributes.size(), 4);
    QCOMPARE(token.attributes.at(0).name, QStringLiteral("href"));
    QCOMPARE(token.attributes.at(0).value, QStringLiteral("/x"));
    QCOMPARE(token.attributes.at(1).name, QStringLiteral("title"));
    QCOMPARE(token.attributes.at(1).value, QStringLiteral("hello"));
    QCOMPARE(token.attributes.at(2).value, QStringLiteral("42"));
    QCOMPARE(token.attributes.at(3).name, QStringLiteral("disabled"));
    QCOMPARE(token.attributes.at(3).value, QString());
}

void HtmlTest::handlesQuotedAndUnquotedAttributes()
{
    // Attribute names are lower-cased; values keep their case.
    html::Tokenizer tokenizer(QStringLiteral("<DIV CLASS=\"MixedCase\">"));
    const html::Token token = tokenizer.nextToken();
    QCOMPARE(token.name, QStringLiteral("div"));
    QCOMPARE(token.attributes.at(0).name, QStringLiteral("class"));
    QCOMPARE(token.attributes.at(0).value, QStringLiteral("MixedCase"));

    // A mismatched or absent close quote still yields a value.
    html::Tokenizer single(QStringLiteral("<div id='abc'>"));
    QCOMPARE(single.nextToken().attributes.at(0).value, QStringLiteral("abc"));
}

void HtmlTest::keepsFirstDuplicateAttribute()
{
    html::Tokenizer tokenizer(QStringLiteral("<div id=\"first\" id=\"second\">"));
    const html::Token token = tokenizer.nextToken();
    QCOMPARE(token.attributes.size(), 1);
    QCOMPARE(token.attributes.at(0).value, QStringLiteral("first"));
}

void HtmlTest::treatsScriptContentAsText()
{
    // A "<" inside a script must not start a tag.
    const QString source = QStringLiteral("<script>if (a < b) { x(); }</script>");
    auto result = html::Parser::parse(source);
    const auto scripts = result.document->getElementsByTagName(QStringLiteral("script"));

    QCOMPARE(scripts.size(), 1);
    QVERIFY(scripts.first()->textContent().contains(QStringLiteral("a < b")));
}

void HtmlTest::treatsStyleContentAsText()
{
    const QString source = QStringLiteral("<style>p > a { color: red; }</style>");
    auto result = html::Parser::parse(source);
    const auto styles = result.document->getElementsByTagName(QStringLiteral("style"));

    QCOMPARE(styles.size(), 1);
    QCOMPARE(styles.first()->textContent(), QStringLiteral("p > a { color: red; }"));
}

void HtmlTest::readsComments()
{
    auto result = html::Parser::parse(QStringLiteral("<div><!-- note --><p>text</p></div>"));

    bool found = false;
    for (dom::Node *node : result.document->descendants()) {
        if (node->isComment()) {
            found = true;
            QCOMPARE(static_cast<dom::Comment *>(node)->data(), QStringLiteral(" note "));
        }
    }
    QVERIFY(found);
}

void HtmlTest::buildsElementTree()
{
    auto result = html::Parser::parse(
        QStringLiteral("<html><body><div id=\"a\"><p class=\"b\">Hi</p></div></body></html>"));

    auto *document = result.document.get();
    QVERIFY(document->documentElement() != nullptr);
    QVERIFY(document->body() != nullptr);

    dom::Element *div = document->getElementById(QStringLiteral("a"));
    QVERIFY(div != nullptr);
    QCOMPARE(div->tagName(), QStringLiteral("div"));

    const auto paragraphs = document->getElementsByClass(QStringLiteral("b"));
    QCOMPARE(paragraphs.size(), 1);
    QCOMPARE(paragraphs.first()->textContent(), QStringLiteral("Hi"));
    QCOMPARE(paragraphs.first()->parentElement(), div);
}

void HtmlTest::createsImpliedElements()
{
    // No <html>, <head> or <body> in the source; the parser must invent them.
    auto result = html::Parser::parse(QStringLiteral("<p>Hello</p>"));

    auto *document = result.document.get();
    QVERIFY(document->documentElement() != nullptr);
    QVERIFY(document->head() != nullptr);
    QVERIFY(document->body() != nullptr);
    QCOMPARE(document->body()->textContent().trimmed(), QStringLiteral("Hello"));
}

void HtmlTest::autoClosesParagraphs()
{
    // A <p> is closed by the next block-level start tag.
    auto result = html::Parser::parse(QStringLiteral("<p>one<p>two<div>three</div>"));

    const auto paragraphs = result.document->getElementsByTagName(QStringLiteral("p"));
    QCOMPARE(paragraphs.size(), 2);
    QCOMPARE(paragraphs.at(0)->textContent(), QStringLiteral("one"));
    QCOMPARE(paragraphs.at(1)->textContent(), QStringLiteral("two"));
    // Neither paragraph swallowed the div.
    QCOMPARE(paragraphs.at(1)->childElements().size(), 0);
}

void HtmlTest::autoClosesListItems()
{
    auto result = html::Parser::parse(QStringLiteral("<ul><li>a<li>b<li>c</ul>"));

    const auto items = result.document->getElementsByTagName(QStringLiteral("li"));
    QCOMPARE(items.size(), 3);
    QCOMPARE(items.at(0)->textContent(), QStringLiteral("a"));
    QCOMPARE(items.at(2)->textContent(), QStringLiteral("c"));
}

void HtmlTest::autoClosesTableCells()
{
    auto result = html::Parser::parse(
        QStringLiteral("<table><tr><td>one<td>two<tr><td>three</table>"));

    const auto cells = result.document->getElementsByTagName(QStringLiteral("td"));
    QCOMPARE(cells.size(), 3);
    QCOMPARE(cells.at(0)->textContent(), QStringLiteral("one"));
    QCOMPARE(cells.at(2)->textContent(), QStringLiteral("three"));

    const auto rows = result.document->getElementsByTagName(QStringLiteral("tr"));
    QCOMPARE(rows.size(), 2);
}

void HtmlTest::handlesUnclosedTags()
{
    // Everything left open is closed at the end of the document.
    auto result = html::Parser::parse(QStringLiteral("<div><span><b>text"));

    QCOMPARE(result.document->body()->textContent(), QStringLiteral("text"));
    QCOMPARE(result.document->getElementsByTagName(QStringLiteral("b")).size(), 1);
    QCOMPARE(result.document->getElementsByTagName(QStringLiteral("div")).size(), 1);
}

void HtmlTest::ignoresStrayEndTags()
{
    auto result = html::Parser::parse(QStringLiteral("<div>text</span></div>"));

    QCOMPARE(result.document->body()->textContent(), QStringLiteral("text"));
    QVERIFY(!result.warnings.isEmpty());
}

void HtmlTest::setsQuirksMode()
{
    auto standards = html::Parser::parse(QStringLiteral("<!DOCTYPE html><p>x</p>"));
    QVERIFY(!standards.document->quirksMode());

    auto quirks = html::Parser::parse(QStringLiteral("<p>x</p>"));
    QVERIFY(quirks.document->quirksMode());

    auto legacy = html::Parser::parse(
        QStringLiteral("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\"><p>x</p>"));
    QVERIFY(legacy.document->quirksMode());
}

void HtmlTest::extractsTitle()
{
    auto result = html::Parser::parse(
        QStringLiteral("<html><head><title>My Page</title></head><body></body></html>"));
    QCOMPARE(result.document->title(), QStringLiteral("My Page"));

    // Without a <title> the first heading is used, as browsers do.
    auto heading = html::Parser::parse(QStringLiteral("<body><h1>Fallback</h1></body>"));
    QCOMPARE(heading.document->title(), QStringLiteral("Fallback"));
}

void HtmlTest::extractsMetadata()
{
    auto result = html::Parser::parse(QStringLiteral(
        "<html><head><meta name=\"description\" content=\"A description\">"
        "<meta charset=\"utf-8\"></head><body></body></html>"));

    QCOMPARE(result.document->metaDescription(), QStringLiteral("A description"));

    const auto metas = result.document->getElementsByTagName(QStringLiteral("meta"));
    QCOMPARE(metas.size(), 2);
    QCOMPARE(metas.at(0)->attribute(QStringLiteral("name")), QStringLiteral("description"));
}

void HtmlTest::serializesBackToHtml()
{
    const QString source = QStringLiteral(
        "<body><div id=\"a\" class=\"b\"><img src=\"x.png\"><p>text &amp; more</p></div></body>");
    auto result = html::Parser::parse(source);

    const QString serialized = result.document->body()->toHtml();
    QVERIFY(serialized.contains(QStringLiteral("<div id=\"a\" class=\"b\">")));
    // A void element has no closing tag.
    QVERIFY(serialized.contains(QStringLiteral("<img src=\"x.png\">")));
    QVERIFY(!serialized.contains(QStringLiteral("</img>")));
    // Text is escaped exactly once.
    QVERIFY(serialized.contains(QStringLiteral("text &amp; more")));
}

void HtmlTest::resolvesUrlsAgainstBase()
{
    auto result = html::Parser::parse(QStringLiteral("<body><a href=\"page\">x</a></body>"),
                                      network::Url::parse(QStringLiteral("https://a.com/dir/")));

    dom::Element *anchor = result.document->getElementsByTagName(QStringLiteral("a")).first();
    QCOMPARE(result.document->resolveUrl(anchor->attribute(QStringLiteral("href"))).toString(),
             QStringLiteral("https://a.com/dir/page"));

    // A <base> element overrides the document URL.
    auto withBase = html::Parser::parse(
        QStringLiteral("<head><base href=\"https://b.com/root/\"></head><body></body></body>"),
        network::Url::parse(QStringLiteral("https://a.com/")));
    QCOMPARE(withBase.document->resolveUrl(QStringLiteral("x")).toString(),
             QStringLiteral("https://b.com/root/x"));
}

QTEST_MAIN(HtmlTest)
#include "tst_html.moc"
