#include <QtTest>

#include "dom/Document.h"
#include "html/Parser.h"
#include "javascript/Engine.h"
#include "javascript/ScriptEngine.h"
#include "javascript/TimerQueue.h"
#include "network/Url.h"

using namespace oqb;
using namespace oqb::javascript;

/// Tests for the JavaScript engine, its DOM bindings and its timers.
///
/// Every case runs a real script against a real parsed document, because the
/// point of the engine is that page code written for a browser works unchanged.
class JavaScriptTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // The engine itself.
    void reportsAvailability();
    void evaluatesExpressions();
    void reportsSyntaxErrors();
    void survivesRuntimeErrors();
    void isolatesRuntimes();
    void drainsPromiseJobs();

    // Reading the document.
    void readsElementsById();
    void readsElementProperties();
    void reflectsAttributes();
    void readsQuerySelectors();
    void readsChildLists();

    // Changing the document.
    void setsAttributes();
    void setsTextContent();
    void createsAndInsertsElements();
    void setsInnerHtml();
    void removesNodesAndKeepsThemUsable();
    void movesNodesBetweenParents();
    void refusesCycles();
    void editsClassList();
    void editsStyleObject();
    void marksDocumentTouched();

    // Events.
    void dispatchesEvents();
    void bubblesEvents();
    void honoursStopPropagation();
    void honoursPreventDefault();
    void runsCapturePhase();
    void removesListeners();
    void runsOnceListeners();
    void reportsListenerErrors();
    void sharesWrapperIdentity();

    // Timers and globals.
    void runsTimeoutsInOrder();
    void clampsNestedTimers();
    void clearsTimeouts();
    void repeatsIntervals();
    void runsAnimationFrames();
    void collectsConsoleOutput();
    void exposesNavigator();

private:
    /// Parses `html` into a document the engine can be bound to.
    void load(const QString &html);

    std::unique_ptr<dom::Document> m_document;
    std::unique_ptr<Engine> m_engine;

    /// Runs `source`, failing the test when it throws.
    QString eval(const QString &source);
};

void JavaScriptTest::init()
{
    // A build without the engine cannot run any of these, so they are skipped
    // rather than reported as failures. Skipping keeps the suite meaningful in
    // both configurations instead of making one of them look broken.
    if (!Engine::isSupported())
        QSKIP("this build has no JavaScript engine (OPENQBROWSER_SCRIPTING=OFF)");

    load(QStringLiteral(
        "<html><head><title>Fixture</title></head><body>"
        "<div id='box' class='outer' data-kind='test'>"
        "<p id='para'>hello</p>"
        "<span class='inner'>world</span>"
        "</div>"
        "<ul id='list'><li>one</li><li>two</li></ul>"
        "<input id='field' type='text'>"
        "</body></html>"));
}

void JavaScriptTest::cleanup()
{
    m_engine.reset();
    m_document.reset();
}

void JavaScriptTest::load(const QString &html)
{
    auto parsed = html::Parser::parse(html, network::Url::parse(QStringLiteral("https://example.com/")));
    QVERIFY(parsed.ok());
    m_document = std::move(parsed.document);
    m_engine = std::make_unique<Engine>(m_document.get());
    QVERIFY(m_engine->isValid());
}

QString JavaScriptTest::eval(const QString &source)
{
    // The snippet is wrapped in a block so that its local declarations do not
    // collide with those of another snippet: top-level code shares one global
    // scope, so `const el` in a second snippet would be a redeclaration error
    // rather than a fresh binding. A block is used rather than a function
    // because a block's completion value is its last statement's, which is what
    // the caller is asserting on.
    const ExecutionResult result
        = m_engine->evaluate(QStringLiteral("{\n%1\n}").arg(source), QStringLiteral("test.js"));
    if (!result.success)
        qWarning() << "script failed:" << source << "\n " << result.error;

    // A failure is reported rather than asserted, because QVERIFY in a
    // value-returning function expands to `return;`, which does not compile.
    // Recording it with QTest lets the calling case fail and carry on.
    if (!result.success) {
        QTest::qFail(qPrintable(result.error), __FILE__, __LINE__);
        return {};
    }
    return result.value;
}

// ------------------------------------------------------------- the engine

void JavaScriptTest::reportsAvailability()
{
    QVERIFY(Engine::isSupported());
    QVERIFY(m_engine->isValid());
    QVERIFY(!Engine::availabilityNote().isEmpty());
    // The version string names the engine and a version, which about:version shows.
    QVERIFY(m_engine->engineVersion().contains(QLatin1String("QuickJS")));
}

void JavaScriptTest::evaluatesExpressions()
{
    QCOMPARE(eval(QStringLiteral("1 + 2")), QStringLiteral("3"));
    QCOMPARE(eval(QStringLiteral("'a' + 'b'")), QStringLiteral("ab"));
    QCOMPARE(eval(QStringLiteral("[1,2,3].length")), QStringLiteral("3"));
    QCOMPARE(eval(QStringLiteral("({a: 1}).a")), QStringLiteral("1"));

    // The parts of the language a page actually depends on.
    QCOMPARE(eval(QStringLiteral("(function () { return 5; })()")), QStringLiteral("5"));
    QCOMPARE(eval(QStringLiteral("(x => x * 2)(4)")), QStringLiteral("8"));
    QCOMPARE(eval(QStringLiteral("`t${1 + 1}`")), QStringLiteral("t2"));
    QCOMPARE(eval(QStringLiteral("[...new Set([1,1,2])].length")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("'abc'.includes('b')")), QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("({...{a:1}, b:2}).b")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("Math.max(...[3,9,4])")), QStringLiteral("9"));
    QCOMPARE(eval(QStringLiteral("JSON.parse('{\"k\":5}').k")), QStringLiteral("5"));
    QCOMPARE(eval(QStringLiteral("class A { get v() { return 7; } }; new A().v")), QStringLiteral("7"));

    // State persists between evaluations, which is what makes separate <script>
    // elements share their declarations.
    eval(QStringLiteral("globalThis.persisted = 11"));
    QCOMPARE(eval(QStringLiteral("persisted")), QStringLiteral("11"));
}

void JavaScriptTest::reportsSyntaxErrors()
{
    const ExecutionResult result = m_engine->evaluate(QStringLiteral("function ("),
                                                     QStringLiteral("broken.js"));
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
    // A syntax error names the script, so a reader can find it.
    QVERIFY(result.error.contains(QLatin1String("broken.js")));
}

void JavaScriptTest::survivesRuntimeErrors()
{
    const ExecutionResult result
        = m_engine->evaluate(QStringLiteral("nonexistentFunction()"), QStringLiteral("boom.js"));
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QLatin1String("nonexistentFunction")));

    // The engine is still usable afterwards, which is what lets one failing
    // script element be reported without stopping the page.
    QCOMPARE(eval(QStringLiteral("2 + 2")), QStringLiteral("4"));
}

void JavaScriptTest::isolatesRuntimes()
{
    // A second engine on its own document must not see the first one's globals.
    auto other = html::Parser::parse(QStringLiteral("<html><body></body></html>"));
    QVERIFY(other.ok());

    Engine second(other.document.get());
    QVERIFY(second.isValid());
    const ExecutionResult isolated = second.evaluate(QStringLiteral("typeof persisted"));
    QVERIFY(isolated.success);
    QCOMPARE(isolated.value, QStringLiteral("undefined"));
}

void JavaScriptTest::drainsPromiseJobs()
{
    // A promise callback runs before evaluate returns, so a page that awaits
    // something small does not need a second turn of the event loop.
    eval(QStringLiteral("globalThis.__settled = 0;"
                        "Promise.resolve(5).then(v => { globalThis.__settled = v; })"));
    QCOMPARE(eval(QStringLiteral("__settled")), QStringLiteral("5"));

    eval(QStringLiteral("(async () => { globalThis.__awaited = await 9; })()"));
    QCOMPARE(eval(QStringLiteral("__awaited")), QStringLiteral("9"));
}

// --------------------------------------------------- reading the document

void JavaScriptTest::readsElementsById()
{
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').tagName")),
             QStringLiteral("DIV"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').id")), QStringLiteral("box"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').className")),
             QStringLiteral("outer"));
    // A missing id is null rather than an error, which script checks against.
    QCOMPARE(eval(QStringLiteral("document.getElementById('nope')")), QStringLiteral("null"));
    QCOMPARE(eval(QStringLiteral("document.getElementsByTagName('li').length")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("document.getElementsByClassName('inner').length")),
             QStringLiteral("1"));
}

void JavaScriptTest::readsElementProperties()
{
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').textContent")),
             QStringLiteral("hello"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').nodeName")),
             QStringLiteral("P"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').nodeType")), QStringLiteral("1"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').parentElement.id")),
             QStringLiteral("box"));
    QCOMPARE(eval(QStringLiteral("document.body.tagName")), QStringLiteral("BODY"));
    QCOMPARE(eval(QStringLiteral("document.documentElement.tagName")), QStringLiteral("HTML"));
    QCOMPARE(eval(QStringLiteral("document.title")), QStringLiteral("Fixture"));
    QCOMPARE(eval(QStringLiteral("document.URL")), QStringLiteral("https://example.com/"));

    // textContent walks the whole subtree, as a browser's does.
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').textContent")),
             QStringLiteral("helloworld"));

    // childNodes counts text nodes too, including the whitespace between elements.
    QVERIFY(eval(QStringLiteral("document.getElementById('list').childNodes.length"))
                .toInt()
            >= 2);
    QCOMPARE(eval(QStringLiteral("document.getElementById('list').children.length")),
             QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('list').firstElementChild.textContent")),
             QStringLiteral("one"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('list').childElementCount")),
             QStringLiteral("2"));
}

void JavaScriptTest::reflectsAttributes()
{
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').getAttribute('data-kind')")),
             QStringLiteral("test"));
    // An absent attribute is null, which script distinguishes from an empty one.
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').getAttribute('missing')")),
             QStringLiteral("null"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').hasAttribute('data-kind')")),
             QStringLiteral("true"));

    QCOMPARE(eval(QStringLiteral("document.getElementById('field').type")),
             QStringLiteral("text"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('field').id")), QStringLiteral("field"));

    // A boolean attribute reflects presence, not text.
    eval(QStringLiteral("document.getElementById('field').disabled = true"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('field').disabled")), QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('field').hasAttribute('disabled')")),
             QStringLiteral("true"));

    eval(QStringLiteral("document.getElementById('field').disabled = false"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('field').disabled")),
             QStringLiteral("false"));

    // Setting a string attribute writes the same lower-cased name the parser uses.
    eval(QStringLiteral("document.getElementById('box').title = 'a title'"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').getAttribute('title')")),
             QStringLiteral("a title"));
}

void JavaScriptTest::readsQuerySelectors()
{
    QCOMPARE(eval(QStringLiteral("document.querySelector('#para').textContent")),
             QStringLiteral("hello"));
    QCOMPARE(eval(QStringLiteral("document.querySelector('div.outer span').textContent")),
             QStringLiteral("world"));
    QCOMPARE(eval(QStringLiteral("document.querySelectorAll('li').length")), QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("document.querySelectorAll('div li').length")), QStringLiteral("0"));
    QCOMPARE(eval(QStringLiteral("document.querySelector('nonexistent')")),
             QStringLiteral("null"));

    // Attribute and child combinators, which real pages lean on.
    QCOMPARE(eval(QStringLiteral("document.querySelector('[data-kind=\"test\"]').id")),
             QStringLiteral("box"));
    QCOMPARE(eval(QStringLiteral("document.querySelector('ul > li').textContent")),
             QStringLiteral("one"));
    QCOMPARE(eval(QStringLiteral("document.querySelector('#box span').parentElement.tagName")),
             QStringLiteral("DIV"));

    // A query from an element searches its descendants only.
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').querySelectorAll('*').length"))
                 .toInt(),
             2);
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').querySelector('p')")),
             QStringLiteral("null"));

    QCOMPARE(eval(QStringLiteral("document.getElementById('para').closest('div').id")),
             QStringLiteral("box"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').matches('div > p')")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("document.body.matches('body')")), QStringLiteral("true"));
}

void JavaScriptTest::readsChildLists()
{
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').children[0].tagName")),
             QStringLiteral("P"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').firstElementChild.id")),
             QStringLiteral("para"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').nextElementSibling.tagName")),
             QStringLiteral("SPAN"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').lastElementChild.tagName")),
             QStringLiteral("SPAN"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').previousElementSibling")),
             QStringLiteral("null"));

    // A forEach over childNodes is the most common way a page walks a list.
    QCOMPARE(eval(QStringLiteral("(() => { let n = 0;"
                                 "document.getElementById('list').childNodes.forEach(() => n++);"
                                 "return n; })()"))
                 .toInt(),
             2);
}

// ------------------------------------------------ changing the document

void JavaScriptTest::setsAttributes()
{
    eval(QStringLiteral("document.getElementById('box').setAttribute('data-added', 'yes')"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').getAttribute('data-added')")),
             QStringLiteral("yes"));
    // The change reaches the DOM, not just the wrapper, so the renderer sees it.
    QCOMPARE(m_document->getElementById(QStringLiteral("box"))
                 ->attribute(QStringLiteral("data-added")),
             QStringLiteral("yes"));

    eval(QStringLiteral("document.getElementById('box').removeAttribute('data-added')"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').hasAttribute('data-added')")),
             QStringLiteral("false"));

    // An attribute name is case-insensitive for HTML, as in a browser.
    eval(QStringLiteral("document.getElementById('box').setAttribute('DATA-UP', 'v')"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').getAttribute('data-up')")),
             QStringLiteral("v"));
}

void JavaScriptTest::setsTextContent()
{
    eval(QStringLiteral("document.getElementById('para').textContent = 'changed'"));
    QCOMPARE(m_document->getElementById(QStringLiteral("para"))->textContent(),
             QStringLiteral("changed"));

    // Replacing the content of an element with children must leave one text node.
    eval(QStringLiteral("document.getElementById('box').textContent = 'flat'"));
    QCOMPARE(m_document->getElementById(QStringLiteral("box"))->textContent(),
             QStringLiteral("flat"));
    QCOMPARE(m_document->getElementById(QStringLiteral("box"))->childCount(), 1);

    // Writing text is escaped when the tree is serialised again, so the text
    // cannot become markup, which is what makes textContent the safe API. A
    // fresh element is used because the snippet above replaced the box's
    // children, and the paragraph is one of them.
    eval(QStringLiteral(
        "const safe = document.createElement('div');"
        "safe.textContent = '<b>x</b>';"
        "document.getElementById('box').appendChild(safe)"));
    QVERIFY(m_document->toHtml().contains(QLatin1String("&lt;b&gt;x&lt;/b&gt;")));
}

void JavaScriptTest::createsAndInsertsElements()
{
    QCOMPARE(eval(QStringLiteral(
                 "const el = document.createElement('a');"
                 "el.href = 'https://example.org/page';"
                 "el.textContent = 'link';"
                 "document.getElementById('box').appendChild(el);"
                 "document.getElementById('box').lastElementChild.textContent")),
             QStringLiteral("link"));

    QCOMPARE(m_document->getElementById(QStringLiteral("box"))->lastElementChild()->tagName(),
             QStringLiteral("a"));
    QCOMPARE(m_document->getElementById(QStringLiteral("box"))
                 ->lastElementChild()
                 ->attribute(QStringLiteral("href")),
             QStringLiteral("https://example.org/page"));

    // insertBefore places a node relative to an existing one.
    QCOMPARE(eval(QStringLiteral(
                 "const li = document.createElement('li');"
                 "li.textContent = 'zero';"
                 "const list = document.getElementById('list');"
                 "list.insertBefore(li, list.firstChild);"
                 "list.firstElementChild.textContent")),
             QStringLiteral("zero"));

    // A text node can be inserted on its own.
    QCOMPARE(eval(QStringLiteral(
                 "const t = document.createTextNode('tail');"
                 "document.getElementById('box').appendChild(t);"
                 "document.getElementById('box').lastChild.nodeType")),
             QStringLiteral("3"));

    // A comment keeps its identity and does not appear as an element.
    QCOMPARE(eval(QStringLiteral(
                 "const c = document.createComment('note');"
                 "document.getElementById('box').appendChild(c);"
                 "document.getElementById('box').lastChild.nodeName")),
             QStringLiteral("#comment"));

    // removeChild and replaceChild both work through the tree.
    QCOMPARE(eval(QStringLiteral(
                 "const list = document.getElementById('list');"
                 "const two = list.lastElementChild;"
                 "const three = document.createElement('li');"
                 "three.textContent = 'three';"
                 "list.replaceChild(three, two);"
                 "list.lastElementChild.textContent")),
             QStringLiteral("three"));
}

void JavaScriptTest::setsInnerHtml()
{
    QCOMPARE(eval(QStringLiteral(
                 "const d = document.createElement('div');"
                 "d.innerHTML = '<p class=\"x\">one</p><p>two</p>';"
                 "d.children.length")),
             QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral(
                 "const d = document.createElement('div');"
                 "d.innerHTML = '<p>one</p>';"
                 "d.firstElementChild.textContent")),
             QStringLiteral("one"));

    // Inner markup is parsed, so attributes come through.
    QCOMPARE(eval(QStringLiteral(
                 "const d = document.createElement('div');"
                 "d.innerHTML = '<a href=\"/x\" id=\"lnk\">go</a>';"
                 "d.querySelector('#lnk').getAttribute('href')")),
             QStringLiteral("/x"));

    // Reading innerHTML back returns markup that parses to the same tree. This
    // is asserted before the body is replaced below, since replacing the body
    // removes the list along with everything else in it.
    const QString markup = eval(QStringLiteral("document.getElementById('list').innerHTML"));
    QVERIFY(markup.contains(QLatin1String("<li")));
    QVERIFY(markup.contains(QLatin1String("one")));

    // Setting a whole document body is the pattern a page uses to render itself.
    eval(QStringLiteral("document.body.innerHTML = '<main id=\"app\"><h1>Rendered</h1></main>'"));
    QCOMPARE(m_document->body()->firstElementChild()->id(), QStringLiteral("app"));
    QCOMPARE(eval(QStringLiteral("document.querySelector('#app h1').textContent")),
             QStringLiteral("Rendered"));

    // The elements that were there before are gone from the document, and the
    // ones script still holds are not, which is the point of the graveyard.
    QCOMPARE(eval(QStringLiteral("document.getElementById('list')")), QStringLiteral("null"));
}

void JavaScriptTest::removesNodesAndKeepsThemUsable()
{
    // This is the case the graveyard exists for: a node taken out of the tree is
    // still reachable from the script that took it out. The reference is kept on
    // the global object, because each snippet is its own scope.
    QCOMPARE(eval(QStringLiteral(
                 "globalThis.__box = document.getElementById('box');"
                 "__box.remove();"
                 "__box.tagName")),
             QStringLiteral("DIV"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box')")), QStringLiteral("null"));

    // The detached node keeps its subtree, and putting it back works.
    QCOMPARE(eval(QStringLiteral(
                 "__box.querySelector('p').textContent")),
             QStringLiteral("hello"));
    QCOMPARE(eval(QStringLiteral(
                 "document.body.appendChild(__box);"
                 "document.getElementById('para').textContent")),
             QStringLiteral("hello"));
    // Putting it back makes it findable by id again.
    QCOMPARE(eval(QStringLiteral("document.getElementById('para').parentElement.id")),
             QStringLiteral("box"));

    // removeChild hands the node to the script just the same.
    QCOMPARE(eval(QStringLiteral(
                 "const list = document.getElementById('list');"
                 "const li = list.firstElementChild;"
                 "list.removeChild(li);"
                 "li.textContent + '/' + list.children.length")),
             QStringLiteral("one/1"));

    // Setting textContent on the parent must not invalidate a reference to a
    // child that was there before.
    QCOMPARE(eval(QStringLiteral(
                 "globalThis.__para = document.getElementById('para');"
                 "document.getElementById('box').textContent = 'replaced';"
                 "__para.textContent")),
             QStringLiteral("hello"));

    // A removed node reports no parent, which script uses to test whether a node
    // is still in the tree.
    QCOMPARE(eval(QStringLiteral(
                 "const li = document.getElementById('list').firstElementChild;"
                 "li.remove();"
                 "String(li.parentNode)")),
             QStringLiteral("null"));
}

void JavaScriptTest::movesNodesBetweenParents()
{
    // Moving a node must not destroy it: the earlier DOM code deleted the node
    // and re-inserted a dangling pointer, which this covers.
    QCOMPARE(eval(QStringLiteral(
                 "const li = document.getElementById('list').firstElementChild;"
                 "document.getElementById('box').appendChild(li);"
                 "document.getElementById('box').lastElementChild.textContent")),
             QStringLiteral("one"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('list').children.length")),
             QStringLiteral("1"));

    // The same node moved back keeps its identity.
    QCOMPARE(eval(QStringLiteral(
                 "const li = document.getElementById('box').lastElementChild;"
                 "const list = document.getElementById('list');"
                 "list.insertBefore(li, list.firstChild);"
                 "list.firstElementChild.textContent")),
             QStringLiteral("one"));

    // Appending an element to itself is a no-op rather than a corruption.
    eval(QStringLiteral("document.getElementById('list').appendChild("
                        "document.getElementById('list').firstElementChild)"));
    QCOMPARE(m_document->getElementById(QStringLiteral("list"))->childElements().size(), 2);
}

void JavaScriptTest::refusesCycles()
{
    // Inserting an ancestor into its own descendant would make the tree a cycle
    // and hang every later walk, so it has to be refused.
    const ExecutionResult result = m_engine->evaluate(
        QStringLiteral("document.getElementById('para').appendChild("
                       "document.getElementById('box'))"),
        QStringLiteral("cycle.js"));
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QLatin1String("ancestor")));

    // The tree is still intact and walkable.
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').children.length")),
             QStringLiteral("2"));
}

void JavaScriptTest::editsClassList()
{
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').classList.length")),
             QStringLiteral("1"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').classList.contains('outer')")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').classList.add('extra');"
                 "document.getElementById('box').className")),
             QStringLiteral("outer extra"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').classList.remove('outer');"
                 "document.getElementById('box').className")),
             QStringLiteral("extra"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').classList.toggle('on');"
                 "document.getElementById('box').classList.contains('on')")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').classList.toggle('on');"
                 "document.getElementById('box').classList.contains('on')")),
             QStringLiteral("false"));

    // The change reaches the DOM, so the cascade will see the new class.
    eval(QStringLiteral("document.getElementById('box').classList.add('visible')"));
    QVERIFY(m_document->getElementById(QStringLiteral("box"))->classList().contains(
        QStringLiteral("visible")));

    // The class attribute is the single source of truth, so change either way.
    eval(QStringLiteral("document.getElementById('box').className = 'a b'"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').classList.length")),
             QStringLiteral("2"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').classList.item(1)")),
             QStringLiteral("b"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').classList.item(5)")),
             QStringLiteral("null"));

    // Adding a class twice does not duplicate it.
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').classList.add('a');"
                 "document.getElementById('box').className")),
             QStringLiteral("a b"));
}

void JavaScriptTest::editsStyleObject()
{
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').style.color = 'red';"
                 "document.getElementById('box').getAttribute('style')")),
             QStringLiteral("color: red"));

    // The camelCase form maps to the dashed property CSS uses.
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').style.backgroundColor = 'blue';"
                 "document.getElementById('box').style.getPropertyValue('background-color')")),
             QStringLiteral("blue"));

    // Writing one property must not disturb another.
    QCOMPARE(eval(QStringLiteral(
                 "const el = document.getElementById('box');"
                 "el.style.getPropertyValue('color')")),
             QStringLiteral("red"));

    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').style.setProperty('margin-top', '4px');"
                 "document.getElementById('box').style.marginTop")),
             QStringLiteral("4px"));

    // An empty value removes the declaration, as the CSSOM specifies.
    eval(QStringLiteral(
        "document.getElementById('box').style.setProperty('color', '')"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('box').style.color")), QString());

    // The declaration reaches the attribute, so the cascade will apply it.
    eval(QStringLiteral("document.getElementById('box').style.display = 'none'"));
    QVERIFY(m_document->getElementById(QStringLiteral("box"))
                ->attribute(QStringLiteral("style"))
                .contains(QLatin1String("display: none")));

    // removeProperty returns the previous value, which script uses to restore it.
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('box').style.removeProperty('display')")),
             QStringLiteral("none"));
}

void JavaScriptTest::marksDocumentTouched()
{
    // A read must not mark the document, or every page would re-lay out forever.
    m_engine->takeDocumentTouched();
    eval(QStringLiteral("document.getElementById('box').tagName"));
    QVERIFY(!m_engine->takeDocumentTouched());

    // A write must, because the renderer has to run again for it to be visible.
    eval(QStringLiteral("document.getElementById('para').textContent = 'new'"));
    QVERIFY(m_engine->takeDocumentTouched());

    // The flag is cleared by reading it, so one change causes one re-layout.
    QVERIFY(!m_engine->takeDocumentTouched());
}

// ----------------------------------------------------------------- events

void JavaScriptTest::dispatchesEvents()
{
    QCOMPARE(eval(QStringLiteral(
                 "let seen = null;"
                 "document.getElementById('para').addEventListener('click', e => { seen = e.type; });"
                 "document.getElementById('para').dispatchEvent(new Event('click'));"
                 "seen")),
             QStringLiteral("click"));

    // The listener receives the event object with its fields set.
    QCOMPARE(eval(QStringLiteral(
                 "let target = null;"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('ping', e => { target = e.target.id; });"
                 "el.dispatchEvent(new Event('ping'));"
                 "target")),
             QStringLiteral("para"));

    QCOMPARE(eval(QStringLiteral(
                 "const el = document.getElementById('para');"
                 "let cur = null;"
                 "el.addEventListener('ping', e => { cur = e.currentTarget.id; });"
                 "el.dispatchEvent(new Event('ping'));"
                 "cur")),
             QStringLiteral("para"));

    // dispatchEvent returns false when a cancelable event was cancelled.
    QCOMPARE(eval(QStringLiteral(
                 "const el = document.getElementById('para');"
                 "el.addEventListener('go', e => e.preventDefault());"
                 "el.dispatchEvent(new Event('go', { cancelable: true }))")),
             QStringLiteral("false"));

    // A non-cancelable event ignores preventDefault, so it cannot be cancelled.
    QCOMPARE(eval(QStringLiteral(
                 "const el = document.getElementById('para');"
                 "el.addEventListener('go2', e => e.preventDefault());"
                 "el.dispatchEvent(new Event('go2'))")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral(
                 "[new Event('a', {bubbles: true}).bubbles, "
                 " new Event('a').bubbles, "
                 " new Event('a', {cancelable: true}).cancelable].join(',')")),
             QStringLiteral("true,false,true"));
}

void JavaScriptTest::bubblesEvents()
{
    // A bubbling event reaches an ancestor listener.
    QCOMPARE(eval(QStringLiteral(
                 "let order = [];"
                 "document.getElementById('box').addEventListener('bub', () => order.push('box'));"
                 "document.getElementById('para').addEventListener('bub', () => order.push('para'));"
                 "document.getElementById('para').dispatchEvent(new Event('bub', { bubbles: true }));"
                 "order.join('>')")),
             QStringLiteral("para>box"));

    // A non-bubbling one stops at the target.
    QCOMPARE(eval(QStringLiteral(
                 "let order = [];"
                 "document.getElementById('box').addEventListener('nb', () => order.push('box'));"
                 "document.getElementById('para').addEventListener('nb', () => order.push('para'));"
                 "document.getElementById('para').dispatchEvent(new Event('nb'));"
                 "order.join('>')")),
             QStringLiteral("para"));

    // Bubbling reaches document.window-level listeners too, which pages rely on
    // for delegation.
    QCOMPARE(eval(QStringLiteral(
                 "let at = null;"
                 "document.addEventListener('del', e => { at = e.target.tagName; });"
                 "document.getElementById('para').dispatchEvent(new Event('del', { bubbles: true }));"
                 "at")),
             QStringLiteral("P"));
}

void JavaScriptTest::honoursStopPropagation()
{
    QCOMPARE(eval(QStringLiteral(
                 "let ran = [];"
                 "document.getElementById('box').addEventListener('s', () => ran.push('box'));"
                 "document.getElementById('para').addEventListener('s', e => { e.stopPropagation(); ran.push('para'); });"
                 "document.getElementById('para').dispatchEvent(new Event('s', { bubbles: true }));"
                 "ran.join('>')")),
             QStringLiteral("para"));

    // stopImmediatePropagation also skips the rest of the listeners on the node.
    QCOMPARE(eval(QStringLiteral(
                 "let ran = [];"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('si', e => { e.stopImmediatePropagation(); ran.push('first'); });"
                 "el.addEventListener('si', () => ran.push('second'));"
                 "el.dispatchEvent(new Event('si'));"
                 "ran.join('>')")),
             QStringLiteral("first"));

    // Without it, both listeners on a node run.
    QCOMPARE(eval(QStringLiteral(
                 "let ran = [];"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('both', () => ran.push('first'));"
                 "el.addEventListener('both', () => ran.push('second'));"
                 "el.dispatchEvent(new Event('both'));"
                 "ran.join('>')")),
             QStringLiteral("first>second"));
}

void JavaScriptTest::honoursPreventDefault()
{
    QCOMPARE(eval(QStringLiteral(
                 "const el = document.getElementById('para');"
                 "let prevented = null;"
                 "el.addEventListener('pd', e => { e.preventDefault(); prevented = e.defaultPrevented; });"
                 "el.dispatchEvent(new Event('pd', { cancelable: true }));"
                 "String(prevented)")),
             QStringLiteral("true"));

    // defaultPrevented is false until preventDefault runs.
    QCOMPARE(eval(QStringLiteral("new Event('x', {cancelable: true}).defaultPrevented")),
             QStringLiteral("false"));
}

void JavaScriptTest::runsCapturePhase()
{
    // Capture runs from the root down, so the ancestor's capture listener runs
    // before the target's listener.
    QCOMPARE(eval(QStringLiteral(
                 "let order = [];"
                 "document.getElementById('box').addEventListener('cap', () => order.push('capture'), true);"
                 "document.getElementById('para').addEventListener('cap', () => order.push('target'));"
                 "document.getElementById('para').dispatchEvent(new Event('cap', { bubbles: true }));"
                 "order.join('>')")),
             QStringLiteral("capture>target"));

    // With capture and bubble listeners on the same ancestor, capture comes first
    // and bubble last.
    QCOMPARE(eval(QStringLiteral(
                 "let order = [];"
                 "const box = document.getElementById('box');"
                 "box.addEventListener('cb', () => order.push('capture'), true);"
                 "box.addEventListener('cb', () => order.push('bubble'));"
                 "document.getElementById('para').dispatchEvent(new Event('cb', { bubbles: true }));"
                 "order.join('>')")),
             QStringLiteral("capture>bubble"));

    // eventPhase reports which phase a listener is in.
    QCOMPARE(eval(QStringLiteral(
                 "let phases = [];"
                 "document.getElementById('box').addEventListener('ph', e => phases.push(e.eventPhase), true);"
                 "document.getElementById('para').addEventListener('ph', e => phases.push(e.eventPhase));"
                 "document.getElementById('para').dispatchEvent(new Event('ph', { bubbles: true }));"
                 "phases.join(',')")),
             QStringLiteral("1,2"));
}

void JavaScriptTest::removesListeners()
{
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "const el = document.getElementById('para');"
                 "const handler = () => count++;"
                 "el.addEventListener('r', handler);"
                 "el.removeEventListener('r', handler);"
                 "el.dispatchEvent(new Event('r'));"
                 "count")),
             QStringLiteral("0"));

    // A different function with the same body is not the same listener, which is
    // why identity is what removeEventListener matches on.
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('r2', () => count++);"
                 "el.removeEventListener('r2', () => count++);"
                 "el.dispatchEvent(new Event('r2'));"
                 "count")),
             QStringLiteral("1"));

    // Removing one of two registrations leaves the other.
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "const el = document.getElementById('para');"
                 "const h = () => count++;"
                 "el.addEventListener('r3', h);"
                 "el.addEventListener('r3', h);"
                 "el.removeEventListener('r3', h);"
                 "el.dispatchEvent(new Event('r3'));"
                 "count")),
             QStringLiteral("1"));

    // A listener that removes itself mid-dispatch does not break the walk.
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "const el = document.getElementById('para');"
                 "const h = () => { count++; el.removeEventListener('r4', h); };"
                 "el.addEventListener('r4', h);"
                 "el.dispatchEvent(new Event('r4'));"
                 "el.dispatchEvent(new Event('r4'));"
                 "count")),
             QStringLiteral("1"));
}

void JavaScriptTest::runsOnceListeners()
{
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('once', () => count++, { once: true });"
                 "el.dispatchEvent(new Event('once'));"
                 "el.dispatchEvent(new Event('once'));"
                 "count")),
             QStringLiteral("1"));

    // A once-listener on an ancestor in the capture phase also runs only once.
    QCOMPARE(eval(QStringLiteral(
                 "let count = 0;"
                 "document.getElementById('box').addEventListener('oncec', () => count++, "
                 "{ once: true, capture: true });"
                 "const el = document.getElementById('para');"
                 "el.dispatchEvent(new Event('oncec', { bubbles: true }));"
                 "el.dispatchEvent(new Event('oncec', { bubbles: true }));"
                 "count")),
             QStringLiteral("1"));
}

void JavaScriptTest::reportsListenerErrors()
{
    // One throwing listener must not stop the others, which is what a browser
    // does: the error is reported and the dispatch carries on.
    QCOMPARE(eval(QStringLiteral(
                 "let ran = [];"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('err', () => { throw new Error('boom'); });"
                 "el.addEventListener('err', () => ran.push('second'));"
                 "el.dispatchEvent(new Event('err'));"
                 "ran.join('>')")),
             QStringLiteral("second"));

    // The error reached the console rather than vanishing.
    const QList<ConsoleMessage> messages = m_engine->messages();
    bool reported = false;
    for (const ConsoleMessage &message : messages) {
        if (message.text.contains(QLatin1String("boom")))
            reported = true;
    }
    QVERIFY(reported);
}

void JavaScriptTest::sharesWrapperIdentity()
{
    // One wrapper per node is what makes these comparisons true, and pages rely
    // on them for de-duplication and comparison.
    QCOMPARE(eval(QStringLiteral("document.body === document.body")), QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('para') === document.getElementById('para')")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral(
                 "document.getElementById('para') === document.getElementById('box')")),
             QStringLiteral("false"));

    // The same object comes back from a query and from a listener's target.
    QCOMPARE(eval(QStringLiteral(
                 "let same = false;"
                 "const el = document.getElementById('para');"
                 "el.addEventListener('id', e => { same = e.target === el; });"
                 "el.dispatchEvent(new Event('id'));"
                 "String(same)")),
             QStringLiteral("true"));

    // instanceof works, which pages use to tell an element from a text node.
    QCOMPARE(eval(QStringLiteral("document.getElementById('para') instanceof Element")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("document.getElementById('para') instanceof Node")),
             QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("new Event('x') instanceof Event")), QStringLiteral("true"));
}

// --------------------------------------------------------------- timers

void JavaScriptTest::runsTimeoutsInOrder()
{
    eval(QStringLiteral(
        "globalThis.__ticks = [];"
        "setTimeout(() => __ticks.push('third'), 30);"
        "setTimeout(() => __ticks.push('first'), 10);"
        "setTimeout(() => __ticks.push('second'), 20);"));

    QVERIFY(!m_engine->timers().isEmpty());
    QVERIFY(m_engine->hasPendingWork());

    // Nothing runs until the caller says the clock has advanced, which is what
    // keeps a callback from firing in the middle of layout.
    QCOMPARE(eval(QStringLiteral("__ticks.length")), QStringLiteral("0"));

    m_engine->runDueTimers(15);
    QCOMPARE(eval(QStringLiteral("__ticks.join('>')")), QStringLiteral("first"));

    m_engine->runDueTimers(1000);
    QCOMPARE(eval(QStringLiteral("__ticks.join('>')")), QStringLiteral("first>second>third"));
    QVERIFY(m_engine->timers().isEmpty());
}

void JavaScriptTest::clampsNestedTimers()
{
    // A zero-delay timer is clamped to at least a millisecond, and one scheduled
    // from inside another timer to the 4ms nested minimum, so that a
    // self-rescheduling callback cannot starve the browser.
    eval(QStringLiteral(
        "globalThis.__n = 0;"
        "setTimeout(() => { __n++; setTimeout(() => { __n++; }, 0); }, 0);"));

    // A 0ms timer is not due at time 0 because of the clamp.
    m_engine->runDueTimers(0);
    QCOMPARE(eval(QStringLiteral("__n")), QStringLiteral("0"));

    m_engine->runDueTimers(10);
    QCOMPARE(eval(QStringLiteral("__n")), QStringLiteral("1"));

    // The timer the callback scheduled is due one millisecond later, since it
    // is nested only one level deep and the floor is still the 1ms minimum.
    m_engine->runDueTimers(11);
    QCOMPARE(eval(QStringLiteral("__n")), QStringLiteral("2"));

    // Past the nesting clamp the floor rises to 4ms, so a chain of self
    // rescheduling callbacks cannot spin the browser. Each nesting level is one
    // timer callback deeper than the last, and the level a callback observes is
    // what the clamp reads.
    eval(QStringLiteral(
        "globalThis.__levels = [];"
        "let depth = 0;"
        "function step() {"
        "  __levels.push(depth);"
        "  if (++depth < 8) setTimeout(step, 0);"
        "}"
        "setTimeout(step, 0)"));

    // Walking the chain forward shows the delay growing once the clamp applies:
    // the early steps are due one millisecond apart, the deeper ones four.
    QStringList dueAt;
    for (int now = 1; now <= 40; ++now) {
        const int before = eval(QStringLiteral("__levels.length")).toInt();
        m_engine->runDueTimers(now);
        const int after = eval(QStringLiteral("__levels.length")).toInt();
        if (after > before)
            dueAt.append(QString::number(now));
    }

    // All eight steps ran, so the chain was not cut short.
    QCOMPARE(eval(QStringLiteral("__levels.length")).toInt(), 8);

    // Consecutive steps stop being one millisecond apart once the nesting level
    // passes the clamp, which is the behaviour being asserted: some gap is
    // greater than one, and no gap is greater than four.
    bool sawLongGap = false;
    for (int i = 1; i < dueAt.size(); ++i) {
        const int gap = dueAt.at(i).toInt() - dueAt.at(i - 1).toInt();
        QVERIFY(gap >= 1);
        QVERIFY(gap <= 4);
        if (gap > 1)
            sawLongGap = true;
    }
    QVERIFY(sawLongGap);
}

void JavaScriptTest::clearsTimeouts()
{
    QCOMPARE(eval(QStringLiteral(
                 "globalThis.__fired = false;"
                 "const id = setTimeout(() => { __fired = true; }, 5);"
                 "clearTimeout(id);"
                 "String(id >= 0)")),
             QStringLiteral("true"));

    QVERIFY(m_engine->timers().isEmpty());
    m_engine->runDueTimers(100);
    QCOMPARE(eval(QStringLiteral("__fired")), QStringLiteral("false"));
}

void JavaScriptTest::repeatsIntervals()
{
    eval(QStringLiteral("globalThis.__count = 0; setInterval(() => __count++, 10)"));

    m_engine->runDueTimers(15);
    QCOMPARE(eval(QStringLiteral("__count")), QStringLiteral("1"));

    m_engine->runDueTimers(25);
    QCOMPARE(eval(QStringLiteral("__count")), QStringLiteral("2"));

    m_engine->runDueTimers(45);
    QCOMPARE(eval(QStringLiteral("__count")), QStringLiteral("4"));

    // An interval stays scheduled until it is cleared.
    QVERIFY(!m_engine->timers().isEmpty());
    eval(QStringLiteral("for (let i = 1; i < 100; i++) clearInterval(i)"));
    QVERIFY(m_engine->timers().isEmpty());
}

void JavaScriptTest::runsAnimationFrames()
{
    eval(QStringLiteral("globalThis.__frames = 0; requestAnimationFrame(() => __frames++)"));

    QCOMPARE(m_engine->timers().pendingAnimationFrames(), 1);
    m_engine->runDueTimers(0);
    QCOMPARE(eval(QStringLiteral("__frames")), QStringLiteral("1"));
}

void JavaScriptTest::collectsConsoleOutput()
{
    const ExecutionResult result = m_engine->evaluate(
        QStringLiteral("console.log('hello', 42); console.warn('careful'); "
                       "console.error('bad'); console.log({a: 1});"),
        QStringLiteral("log.js"));

    QCOMPARE(result.messages.size(), 4);
    QCOMPARE(result.messages.at(0).text, QStringLiteral("hello 42"));
    QCOMPARE(result.messages.at(0).level, ConsoleMessage::Level::Log);
    QCOMPARE(result.messages.at(1).text, QStringLiteral("careful"));
    QCOMPARE(result.messages.at(1).level, ConsoleMessage::Level::Warning);
    QCOMPARE(result.messages.at(2).level, ConsoleMessage::Level::Error);
    // An object is shown as JSON rather than as the string "[object Object]".
    QCOMPARE(result.messages.at(3).text, QStringLiteral("{\"a\":1}"));

    // The messages carry the script they came from, which the console shows.
    QCOMPARE(result.messages.at(0).source, QStringLiteral("log.js"));
}

void JavaScriptTest::exposesNavigator()
{
    QVERIFY(eval(QStringLiteral("navigator.userAgent")).contains(QLatin1String("OpenQBrowser")));
    QCOMPARE(eval(QStringLiteral("navigator.language")), QStringLiteral("en-US"));
    // The one property that picks a page's code path: claiming true would send a
    // page down its automation branch.
    QCOMPARE(eval(QStringLiteral("navigator.webdriver")), QStringLiteral("false"));

    // window, self and the global object are one thing.
    QCOMPARE(eval(QStringLiteral("window === globalThis")), QStringLiteral("true"));
    QCOMPARE(eval(QStringLiteral("self === window")), QStringLiteral("true"));

    // The location reflects the document's URL rather than being invented.
    QCOMPARE(eval(QStringLiteral("location.host")), QStringLiteral("example.com"));
    QCOMPARE(eval(QStringLiteral("location.protocol")), QStringLiteral("https:"));
    QCOMPARE(eval(QStringLiteral("location.pathname")), QStringLiteral("/"));

    // btoa and atob are the pair a page uses for a basic-auth header.
    QCOMPARE(eval(QStringLiteral("btoa('user:pw')")), QStringLiteral("dXNlcjpwdw=="));
    QCOMPARE(eval(QStringLiteral("atob('dXNlcjpwdw==')")), QStringLiteral("user:pw"));

    // readyState starts as loading, which a page waits to change.
    QCOMPARE(eval(QStringLiteral("document.readyState")), QStringLiteral("loading"));
}

QTEST_MAIN(JavaScriptTest)
#include "tst_javascript.moc"
