#include <QtTest>

#include "css/Style.h"
#include "css/Stylesheet.h"
#include "css/Tokenizer.h"
#include "css/Value.h"
#include "dom/Document.h"
#include "html/Parser.h"

using namespace oqb;

/// Tests for the CSS tokenizer, value parsing, selectors, stylesheet parsing
/// and the cascade.
class CssTest : public QObject
{
    Q_OBJECT

private slots:
    // Tokenizer
    void tokenizesIdentifiersAndNumbers();
    void tokenizesStringsAndUrls();
    void tokenizesSymbols();
    void ignoresComments();
    // Values
    void parsesHexColors_data();
    void parsesHexColors();
    void parsesColorFunctions();
    void parsesNamedColors();
    void parsesLengthUnits();
    // Selectors
    void parsesSelectorLists();
    void computesSpecificity();
    void matchesCombinators();
    void matchesAttributeSelectors();
    void matchesPseudoClasses();
    void splitsSelectorListsOnTopLevelCommas();
    // Stylesheet
    void parsesRulesAndDeclarations();
    void parsesImportantDeclarations();
    void parsesShorthands();
    void recoversFromMalformedInput();
    void parsesMediaQueries();
    void evaluatesMediaQueries();
    // Cascade
    void appliesInheritance();
    void appliesSpecificity();
    void appliesImportantAndOriginOrder();
    void appliesInlineStyles();
    void computesRelativeFontSizes();
    void appliesUserAgentDefaults();
};

void CssTest::tokenizesIdentifiersAndNumbers()
{
    css::Tokenizer tokenizer(QStringLiteral("color 12px 50% 1.5 -3em .5"));

    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Whitespace);

    css::Token px = tokenizer.nextToken();
    QCOMPARE(px.type, css::TokenType::Dimension);
    QCOMPARE(px.number, 12.0);
    QCOMPARE(px.unit, QStringLiteral("px"));

    tokenizer.nextToken(); // whitespace
    css::Token percent = tokenizer.nextToken();
    QCOMPARE(percent.type, css::TokenType::Percentage);
    QCOMPARE(percent.number, 50.0);

    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Number);

    tokenizer.nextToken();
    css::Token negative = tokenizer.nextToken();
    QCOMPARE(negative.type, css::TokenType::Dimension);
    QCOMPARE(negative.number, -3.0);
    QCOMPARE(negative.unit, QStringLiteral("em"));

    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().number, 0.5);
}

void CssTest::tokenizesStringsAndUrls()
{
    css::Tokenizer quotes(QStringLiteral("\"double\" 'single'"));
    QCOMPARE(quotes.nextToken().value, QStringLiteral("double"));
    quotes.nextToken();
    QCOMPARE(quotes.nextToken().value, QStringLiteral("single"));

    css::Tokenizer bareUrl(QStringLiteral("url(image.png)"));
    css::Token url = bareUrl.nextToken();
    QCOMPARE(url.type, css::TokenType::Url);
    QCOMPARE(url.value, QStringLiteral("image.png"));

    css::Tokenizer quotedUrl(QStringLiteral("url('with space.png')"));
    QCOMPARE(quotedUrl.nextToken().value, QStringLiteral("with space.png"));
}

void CssTest::tokenizesSymbols()
{
    css::Tokenizer tokenizer(QStringLiteral("a { b: c; }"));
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::LeftCurly);
    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Colon);
    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Semicolon);
    tokenizer.nextToken();
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::RightCurly);
}

void CssTest::ignoresComments()
{
    css::Tokenizer tokenizer(QStringLiteral("a /* comment */ b"));
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
    // The comment is skipped entirely; whitespace before and after merges.
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Whitespace);
    QCOMPARE(tokenizer.nextToken().type, css::TokenType::Ident);
}

void CssTest::parsesHexColors_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("6 digits") << "#ff0000" << "#ff0000";
    QTest::newRow("3 digits") << "#f00" << "#ff0000";
    QTest::newRow("mixed case") << "#AbCdEf" << "#abcdef";
    QTest::newRow("with alpha") << "#00ff0080" << "#00ff00";
    QTest::newRow("3 digits with alpha") << "#f00f" << "#ff0000";
}

void CssTest::parsesHexColors()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    const css::Value color = css::values::parseColor(input);
    QVERIFY(color.isColor());
    QCOMPARE(color.color.name(QColor::HexRgb), expected);
}

void CssTest::parsesColorFunctions()
{
    QCOMPARE(css::values::parseColor(QStringLiteral("rgb(255, 0, 0)")).color.name(QColor::HexRgb),
             QStringLiteral("#ff0000"));
    QCOMPARE(css::values::parseColor(QStringLiteral("rgb(100%, 0%, 0%)")).color.name(QColor::HexRgb),
             QStringLiteral("#ff0000"));
    QCOMPARE(css::values::parseColor(QStringLiteral("hsl(0, 100%, 50%)")).color.name(QColor::HexRgb),
             QStringLiteral("#ff0000"));
    QCOMPARE(css::values::parseColor(QStringLiteral("hsl(120, 100%, 25%)")).color.name(QColor::HexRgb),
             QStringLiteral("#008000"));

    const css::Value translucent = css::values::parseColor(QStringLiteral("rgba(0, 0, 255, 0.5)"));
    QVERIFY(translucent.isColor());
    QCOMPARE(translucent.color.alpha(), 128);

    // The modern space-separated syntax with a slash alpha.
    QCOMPARE(css::values::parseColor(QStringLiteral("rgb(0 128 0 / 50%)")).color.name(QColor::HexRgb),
             QStringLiteral("#008000"));

    QVERIFY(!css::values::parseColor(QStringLiteral("rgb(1, 2)")).isValid());
    QVERIFY(!css::values::parseColor(QStringLiteral("#xyz")).isValid());
    QVERIFY(!css::values::parseColor(QStringLiteral("not-a-color")).isValid());
}

void CssTest::parsesNamedColors()
{
    QCOMPARE(css::values::parseColor(QStringLiteral("red")).color.name(QColor::HexRgb),
             QStringLiteral("#ff0000"));
    QCOMPARE(css::values::parseColor(QStringLiteral("rebeccapurple")).color.name(QColor::HexRgb),
             QStringLiteral("#663399"));
    QCOMPARE(css::values::parseColor(QStringLiteral("TRANSPARENT")).color.alpha(), 0);
    // currentColor depends on the element, so it is not resolved here.
    QVERIFY(!css::values::parseColor(QStringLiteral("currentColor")).isValid());
}

void CssTest::parsesLengthUnits()
{
    double pixels = 0;

    QVERIFY(css::values::lengthToPixels(10, QStringLiteral("px"), 16, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 10.0);

    // Absolute units convert at 96 dpi.
    QVERIFY(css::values::lengthToPixels(1, QStringLiteral("in"), 16, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 96.0);
    QVERIFY(css::values::lengthToPixels(72, QStringLiteral("pt"), 16, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 96.0);

    // Relative units use the supplied context.
    QVERIFY(css::values::lengthToPixels(2, QStringLiteral("em"), 20, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 40.0);
    QVERIFY(css::values::lengthToPixels(2, QStringLiteral("rem"), 20, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 32.0);
    QVERIFY(css::values::lengthToPixels(50, QStringLiteral("vw"), 16, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 500.0);
    QVERIFY(css::values::lengthToPixels(50, QStringLiteral("vh"), 16, 16, 1000, 800, &pixels));
    QCOMPARE(pixels, 400.0);

    // An unknown unit is rejected rather than guessed at.
    QVERIFY(!css::values::lengthToPixels(1, QStringLiteral("furlong"), 16, 16, 1000, 800, &pixels));
}

void CssTest::parsesSelectorLists()
{
    const auto selectors = css::SelectorParser::parseList(
        QStringLiteral("p.note, #main > a:hover, ul li"));

    QCOMPARE(selectors.size(), 3);
    QVERIFY(selectors.at(0).isValid());
    QVERIFY(selectors.at(1).isValid());
    QVERIFY(selectors.at(2).isValid());
    QCOMPARE(selectors.at(0).steps.size(), 1);
    QCOMPARE(selectors.at(1).steps.size(), 2);
    QCOMPARE(selectors.at(2).steps.size(), 2);
}

void CssTest::computesSpecificity()
{
    const auto specificity = [](const QString &text) {
        return css::SelectorParser::parse(text).specificity();
    };

    // IDs beat classes beat types.
    QVERIFY(specificity(QStringLiteral("#a")) > specificity(QStringLiteral(".a")));
    QVERIFY(specificity(QStringLiteral(".a")) > specificity(QStringLiteral("a")));
    QVERIFY(specificity(QStringLiteral("a.b")) > specificity(QStringLiteral("a")));
    QVERIFY(specificity(QStringLiteral("a b")) > specificity(QStringLiteral("a")));
    // The universal selector contributes nothing.
    QCOMPARE(specificity(QStringLiteral("*")), 0u);
    // An id beats any number of classes.
    QVERIFY(specificity(QStringLiteral("#a")) > specificity(QStringLiteral(".a.b.c.d.e")));
}

void CssTest::matchesCombinators()
{
    auto result = html::Parser::parse(QStringLiteral(
        "<body><div id=\"outer\"><p id=\"p1\">a</p><span id=\"s1\">b</span>"
        "<p id=\"p2\">c</p></div></body>"));
    auto *document = result.document.get();

    const auto matches = [&](const QString &selector, const QString &id) {
        return css::SelectorParser::parse(selector).matches(document->getElementById(id));
    };

    QVERIFY(matches(QStringLiteral("div p"), QStringLiteral("p1")));
    QVERIFY(matches(QStringLiteral("div > p"), QStringLiteral("p1")));
    QVERIFY(!matches(QStringLiteral("body > p"), QStringLiteral("p1")));
    QVERIFY(matches(QStringLiteral("p + span"), QStringLiteral("s1")));
    QVERIFY(!matches(QStringLiteral("p + p"), QStringLiteral("p2")));
    QVERIFY(matches(QStringLiteral("span ~ p"), QStringLiteral("p2")));
    QVERIFY(matches(QStringLiteral("#outer #p1"), QStringLiteral("p1")));
    QVERIFY(!matches(QStringLiteral("#p2"), QStringLiteral("p1")));
}

void CssTest::matchesAttributeSelectors()
{
    auto result = html::Parser::parse(QStringLiteral(
        "<body><a id=\"a1\" href=\"https://x.com/path.pdf\" hreflang=\"en-US\" class=\"btn primary\">x</a></body>"));
    auto *document = result.document.get();
    auto *anchor = document->getElementById(QStringLiteral("a1"));

    const auto matches = [&](const QString &selector) {
        return css::SelectorParser::parse(selector).matches(anchor);
    };

    QVERIFY(matches(QStringLiteral("[href]")));
    QVERIFY(!matches(QStringLiteral("[missing]")));
    QVERIFY(matches(QStringLiteral("[class~=\"btn\"]")));
    QVERIFY(!matches(QStringLiteral("[class~=\"bt\"]")));
    QVERIFY(matches(QStringLiteral("[hreflang|=\"en\"]")));
    QVERIFY(matches(QStringLiteral("[href^=\"https\"]")));
    QVERIFY(matches(QStringLiteral("[href$=\".pdf\"]")));
    QVERIFY(matches(QStringLiteral("[href*=\"x.com\"]")));
    QVERIFY(matches(QStringLiteral("[class~=\"BTN\" i]"))); // case-insensitive flag
    QVERIFY(!matches(QStringLiteral("[class~=\"BTN\"]")));
}

void CssTest::matchesPseudoClasses()
{
    auto result = html::Parser::parse(QStringLiteral(
        "<body><ul id=\"list\"><li id=\"first\">1</li><li id=\"second\" class=\"pick\">2</li>"
        "<li id=\"third\">3</li></ul></body>"));
    auto *document = result.document.get();

    const auto matches = [&](const QString &selector, const QString &id) {
        return css::SelectorParser::parse(selector).matches(document->getElementById(id));
    };

    QVERIFY(matches(QStringLiteral("li:first-child"), QStringLiteral("first")));
    QVERIFY(!matches(QStringLiteral("li:first-child"), QStringLiteral("second")));
    QVERIFY(matches(QStringLiteral("li:last-child"), QStringLiteral("third")));
    QVERIFY(matches(QStringLiteral("li:nth-child(2)"), QStringLiteral("second")));
    QVERIFY(matches(QStringLiteral("li:nth-child(odd)"), QStringLiteral("first")));
    QVERIFY(!matches(QStringLiteral("li:nth-child(odd)"), QStringLiteral("second")));
    QVERIFY(matches(QStringLiteral("li:nth-child(even)"), QStringLiteral("second")));
    QVERIFY(matches(QStringLiteral("li:nth-child(2n+2)"), QStringLiteral("second")));
    QVERIFY(!matches(QStringLiteral("li:not(.pick)"), QStringLiteral("second")));
    QVERIFY(matches(QStringLiteral("li:not(.pick)"), QStringLiteral("first")));
    // nth-last-child counts from the end.
    QVERIFY(matches(QStringLiteral("li:nth-last-child(1)"), QStringLiteral("third")));
}

void CssTest::splitsSelectorListsOnTopLevelCommas()
{
    QCOMPARE(css::splitSelectorList(QStringLiteral("a, b, c")).size(), 3);
    // A comma inside :not() or an attribute value does not split.
    QCOMPARE(css::splitSelectorList(QStringLiteral("a:not(.x, .y), b")).size(), 2);
    QCOMPARE(css::splitSelectorList(QStringLiteral("[title=\"a,b\"], c")).size(), 2);
}

void CssTest::parsesRulesAndDeclarations()
{
    const auto sheet = css::Stylesheet::parse(QStringLiteral(
        "p { color: red; font-size: 20px } .a { margin: 5px 10px }"));

    QCOMPARE(sheet.rules().size(), 2);
    QCOMPARE(sheet.rules().at(0).declarations.size(), 2);
    QCOMPARE(sheet.rules().at(0).declarations.at(0).property, QStringLiteral("color"));
    QVERIFY(sheet.rules().at(0).declarations.at(0).value.isColor());
    QCOMPARE(sheet.rules().at(0).declarations.at(1).property, QStringLiteral("font-size"));
    QCOMPARE(sheet.rules().at(1).selectorText(), QStringLiteral(".a"));
}

void CssTest::parsesImportantDeclarations()
{
    const auto sheet = css::Stylesheet::parse(
        QStringLiteral("p { color: red !important; background: blue }"));

    QVERIFY(sheet.rules().at(0).declarations.at(0).important);
    QVERIFY(!sheet.rules().at(0).declarations.at(1).important);
    // The flag is stripped from the value itself.
    QVERIFY(sheet.rules().at(0).declarations.at(0).value.isColor());
}

void CssTest::parsesShorthands()
{
    const auto sheet = css::Stylesheet::parse(QStringLiteral("p { margin: 1px 2px 3px 4px }"));
    QCOMPARE(sheet.rules().at(0).declarations.size(), 1);
    QCOMPARE(sheet.rules().at(0).declarations.at(0).property, QStringLiteral("margin"));

    // The cascade expands it into the four longhands, which is checked by the
    // layout test; here the raw text is what matters.
    const css::Value value = sheet.rules().at(0).declarations.at(0).value;
    QVERIFY(value.toString().contains(QLatin1String("1px")));
}

void CssTest::recoversFromMalformedInput()
{
    QStringList errors;
    const auto sheet = css::Stylesheet::parse(QStringLiteral(
        "p { color: red }\n"
        "this is not a rule\n"
        "div { font-size: }\n"
        "span { color: blue; }\n"
        "@unknown-block { whatever }\n"
        "a { color: green }"), 0, 0, &errors);

    // The rules with usable declarations survive; the ones with none are
    // dropped, because an empty rule block contributes nothing to the cascade.
    QCOMPARE(sheet.rules().size(), 2);
    QCOMPARE(sheet.rules().at(0).selectorText(), QStringLiteral("p"));
    QCOMPARE(sheet.rules().at(1).selectorText(), QStringLiteral("span"));
    QCOMPARE(sheet.rules().at(1).declarations.size(), 1);
    QCOMPARE(sheet.rules().at(1).declarations.at(0).property, QStringLiteral("color"));

    // "div { font-size: }" has no value, so it produces no declaration; the
    // parser reports the problem and carries on with the next rule.
    QVERIFY(!errors.isEmpty());
}

void CssTest::parsesMediaQueries()
{
    const auto sheet = css::Stylesheet::parse(QStringLiteral(
        "@media (min-width: 600px) { p { color: red } }\n"
        "div { color: blue }"));

    // The nested rule is kept in the at-rule and flattened when queried.
    QCOMPARE(sheet.atRules().size(), 1);
    QCOMPARE(sheet.atRules().at(0).name, QStringLiteral("media"));
    QCOMPARE(sheet.atRules().at(0).rules.size(), 1);

    // At 800px wide the media rule applies, so both rules are returned.
    const auto wide = sheet.rulesForMedia(QStringLiteral("screen"), 800);
    QCOMPARE(wide.size(), 2);

    // At 400px it does not.
    const auto narrow = sheet.rulesForMedia(QStringLiteral("screen"), 400);
    QCOMPARE(narrow.size(), 1);
}

void CssTest::evaluatesMediaQueries()
{
    QVERIFY(css::MediaQuery::matches(QStringLiteral("(min-width: 600px)"), 800, 600));
    QVERIFY(!css::MediaQuery::matches(QStringLiteral("(min-width: 600px)"), 400, 600));
    QVERIFY(css::MediaQuery::matches(QStringLiteral("(max-width: 600px)"), 400, 600));
    QVERIFY(css::MediaQuery::matches(QStringLiteral("screen"), 800, 600));
    QVERIFY(!css::MediaQuery::matches(QStringLiteral("print"), 800, 600));
    QVERIFY(css::MediaQuery::matches(QStringLiteral("all"), 800, 600));
    QVERIFY(css::MediaQuery::matches(QStringLiteral("(orientation: landscape)"), 800, 600));
    QVERIFY(!css::MediaQuery::matches(QStringLiteral("(orientation: portrait)"), 800, 600));
    // A comma separated list matches when any term does.
    QVERIFY(css::MediaQuery::matches(QStringLiteral("print, screen"), 800, 600));
    // An unknown feature never matches, rather than throwing.
    QVERIFY(!css::MediaQuery::matches(QStringLiteral("(unknown-feature: 1)"), 800, 600));
    // An empty query means "no restriction".
    QVERIFY(css::MediaQuery::matches(QString(), 800, 600));
}

void CssTest::appliesInheritance()
{
    auto result = html::Parser::parse(
        QStringLiteral("<body><div><p>text</p></div></body>"));
    auto *document = result.document.get();

    css::StyleContext context;
    css::StyleEngine engine(context);
    engine.addStylesheet(css::Stylesheet::parse(
        QStringLiteral("body { color: rgb(1,2,3); font-size: 21px; font-style: italic }")));
    engine.computeStyles(document);

    auto *paragraph = document->getElementsByTagName(QStringLiteral("p")).first();

    // Colour, font size and italics all inherit down to the paragraph.
    QCOMPARE(engine.styleFor(paragraph).color, QColor(1, 2, 3));
    QCOMPARE(engine.styleFor(paragraph).fontSize, 21.0);
    QVERIFY(engine.styleFor(paragraph).italic);

    // Margin does not inherit: the paragraph keeps its own UA value.
    QCOMPARE(engine.styleFor(document->body()).marginTop.value, 8.0);
}

void CssTest::appliesSpecificity()
{
    auto result = html::Parser::parse(
        QStringLiteral("<body><p id=\"x\" class=\"a\">t</p></body>"));
    auto *document = result.document.get();

    css::StyleEngine engine;
    engine.addStylesheet(css::Stylesheet::parse(QStringLiteral(
        "p { color: red } .a { color: green } #x { color: blue }")));
    engine.computeStyles(document);

    auto *paragraph = document->getElementById(QStringLiteral("x"));
    // The id selector wins.
    QCOMPARE(engine.styleFor(paragraph).color, QColor(Qt::blue));
}

void CssTest::appliesImportantAndOriginOrder()
{
    auto result = html::Parser::parse(QStringLiteral("<body><p id=\"x\">t</p></body>"));
    auto *document = result.document.get();

    css::StyleEngine engine;
    // !important beats a higher-specificity selector without it, even though
    // the important rule's selector is only an element selector.
    engine.addStylesheet(css::Stylesheet::parse(QStringLiteral(
        "#x { color: blue } p { color: red !important }")));
    engine.computeStyles(document);

    QCOMPARE(engine.styleFor(document->getElementById(QStringLiteral("x"))).color,
             QColor(Qt::red));
}

void CssTest::appliesInlineStyles()
{
    auto result = html::Parser::parse(
        QStringLiteral("<body><p id=\"x\" style=\"color: purple\">t</p></body>"));
    auto *document = result.document.get();

    css::StyleEngine engine;
    // An inline style outranks any selector.
    engine.addStylesheet(css::Stylesheet::parse(QStringLiteral("#x { color: blue }")));
    engine.computeStyles(document);

    QCOMPARE(engine.styleFor(document->getElementById(QStringLiteral("x"))).color,
             QColor(128, 0, 128));
}

void CssTest::computesRelativeFontSizes()
{
    auto result = html::Parser::parse(
        QStringLiteral("<body><div><span>t</span></div></body>"));
    auto *document = result.document.get();

    css::StyleEngine engine;
    engine.addStylesheet(css::Stylesheet::parse(QStringLiteral(
        "body { font-size: 20px } div { font-size: 2em } span { font-size: 50% }")));
    engine.computeStyles(document);

    // 20px -> 40px -> 20px: relative sizes compound through the tree.
    QCOMPARE(engine.styleFor(document->body()).fontSize, 20.0);
    QCOMPARE(engine.styleFor(document->getElementsByTagName(QStringLiteral("div")).first()).fontSize,
             40.0);
    QCOMPARE(engine.styleFor(document->getElementsByTagName(QStringLiteral("span")).first()).fontSize,
             20.0);
}

void CssTest::appliesUserAgentDefaults()
{
    auto result = html::Parser::parse(
        QStringLiteral("<body><h1>t</h1><ul><li>i</li></ul><a href=\"#\">a</a></body>"));
    auto *document = result.document.get();

    css::StyleEngine engine;
    engine.computeStyles(document);

    // The UA sheet gives headings a large bold font and lists their markers.
    auto *heading = document->getElementsByTagName(QStringLiteral("h1")).first();
    QVERIFY(engine.styleFor(heading).fontSize > 16.0);
    QVERIFY(engine.styleFor(heading).fontWeight >= 700);
    QVERIFY(engine.styleFor(heading).isBlockLevel());

    auto *item = document->getElementsByTagName(QStringLiteral("li")).first();
    QCOMPARE(engine.styleFor(item).display, QStringLiteral("list-item"));

    // Links get the link colour and an underline.
    auto *anchor = document->getElementsByTagName(QStringLiteral("a")).first();
    QCOMPARE(engine.styleFor(anchor).color, QColor(0, 0, 238));
    QVERIFY(engine.styleFor(anchor).textDecoration.contains(QLatin1String("underline")));

    // An anchor with no href is not a link and does not get those styles.
    auto plain = html::Parser::parse(QStringLiteral("<body><a>plain</a></body>"));
    css::StyleEngine plainEngine;
    plainEngine.computeStyles(plain.document.get());
    auto *plainAnchor = plain.document->getElementsByTagName(QStringLiteral("a")).first();
    QVERIFY(!plainEngine.styleFor(plainAnchor).textDecoration.contains(QLatin1String("underline")));
}

QTEST_MAIN(CssTest)
#include "tst_css.moc"
