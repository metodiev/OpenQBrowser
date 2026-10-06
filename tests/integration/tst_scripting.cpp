#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include "browser/Page.h"
#include "devtools/Inspector.h"
#include "dom/Document.h"
#include "javascript/Engine.h"
#include "network/Url.h"

using namespace oqb;
using namespace oqb::browser;

/// Tests for page scripts running inside the real load pipeline.
///
/// The unit tests cover the engine and its bindings in isolation; these cover
/// the part that only shows up when a page actually loads: that scripts are
/// discovered, fetched, run in the right order, given the chance to change the
/// document before it is laid out, and that the page still finishes loading.
class ScriptingTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void runsInlineScriptBeforeLayout();
    void runsExternalScript();
    void runsDeferredScriptAfterInline();
    void ignoresUnknownScriptTypes();
    void reportsScriptErrors();
    void collectsConsoleOutput();
    void finishesWhenTheOnlySubresourceIsAScript();
    void firesDomContentLoaded();
    void firesLoadEvent();
    void runsTimersAndRelaysOut();
    void survivesAScriptThatRemovesItself();
    void handlesAnInlineScriptWithNoContent();

private:
    /// Writes `html` to a temporary file and loads it through a real Page.
    /// A file:// URL is used because it needs no network and no test server, and
    /// it exercises the same code path a downloaded page takes.
    bool loadPage(const QString &html, const QDir &directory);

    std::unique_ptr<Page> m_page;
    QTemporaryDir m_directory;
};

bool ScriptingTest::loadPage(const QString &html, const QDir &directory)
{
    QFile file(directory.filePath(QStringLiteral("index.html")));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(html.toUtf8());
    file.close();

    PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;
    settings.loadImages = true;

    m_page = std::make_unique<Page>(settings);

    QSignalSpy finishedSpy(m_page.get(), &Page::finished);
    m_page->load(network::Url::fromLocalFile(file.fileName()));

    // Loading runs off the event loop, so the loop is pumped until the page
    // reports that it is done. A local file completes quickly, so the timeout is
    // only a guard against a regression that stops it finishing at all.
    QElapsedTimer clock;
    clock.start();
    while (finishedSpy.isEmpty() && clock.elapsed() < 15000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    return !finishedSpy.isEmpty();
}

void ScriptingTest::initTestCase()
{
    // Without an engine no page runs a script, so these cases are skipped rather
    // than failing. The behaviour of that build is covered by the unit tests'
    // availability check.
    if (!javascript::Engine::isSupported())
        QSKIP("this build has no JavaScript engine (OPENQBROWSER_SCRIPTING=OFF)");
}

// ------------------------------------------------------------------ script execution

void ScriptingTest::runsInlineScriptBeforeLayout()
{
    QVERIFY(m_directory.isValid());
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='a'>before</div>"
                         "<script>document.getElementById('a').textContent = 'after';</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    // The script ran, so the DOM carries the new text.
    QVERIFY(m_page->document() != nullptr);
    QCOMPARE(m_page->document()->getElementById(QStringLiteral("a"))->textContent(),
             QStringLiteral("after"));

    // The boxes were built after the script ran, so the text that would be
    // painted is the new one. A script whose change never reached the layout
    // would leave the old text in the box tree, which is the bug this covers.
    QVERIFY(m_page->boxTree() != nullptr);
    const QString painted = devtools::Inspector::boxTree(m_page->boxTree());
    QVERIFY2(painted.contains(QLatin1String("after")), qPrintable(painted));
    QVERIFY(!painted.contains(QLatin1String("before")));
}

void ScriptingTest::runsExternalScript()
{
    QVERIFY(m_directory.isValid());
    QDir directory(m_directory.path());

    QFile script(directory.filePath(QStringLiteral("page.js")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("document.getElementById('out').textContent = 'from the file';");
    script.close();

    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='out'>nothing</div>"
                         "<script src='page.js'></script>"
                         "</body></html>"),
                     directory));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("out"))->textContent(),
             QStringLiteral("from the file"));
}

void ScriptingTest::runsDeferredScriptAfterInline()
{
    QVERIFY(m_directory.isValid());
    QDir directory(m_directory.path());

    // The deferred script appends to a value the inline script sets, so the
    // result is only correct if the order was inline first, deferred second.
    QFile deferred(directory.filePath(QStringLiteral("deferred.js")));
    QVERIFY(deferred.open(QIODevice::WriteOnly));
    deferred.write("document.getElementById('seq').textContent += '|deferred';");
    deferred.close();

    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='seq'>start</div>"
                         "<script>document.getElementById('seq').textContent += '|inline';</script>"
                         "<script defer src='deferred.js'></script>"
                         "</body></html>"),
                     directory));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("seq"))->textContent(),
             QStringLiteral("start|inline|deferred"));
}

void ScriptingTest::ignoresUnknownScriptTypes()
{
    QVERIFY(m_directory.isValid());

    // A data block is not code. Running it would be both wrong and unsafe, since
    // a page may put arbitrary text there.
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='safe'>untouched</div>"
                         "<script type='application/json'>{ \"a\": 1 }</script>"
                         "<script type='text/template'><b>markup</b></script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("safe"))->textContent(),
             QStringLiteral("untouched"));
}

void ScriptingTest::reportsScriptErrors()
{
    QVERIFY(m_directory.isValid());

    // A throwing script must not stop the page loading, and the error must reach
    // the console rather than disappearing.
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='ok'>fine</div>"
                         "<script>throw new Error('script failed on purpose');</script>"
                         "<script>document.getElementById('ok').textContent = 'still ran';</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    // The script after the failing one still ran.
    QCOMPARE(m_page->document()->getElementById(QStringLiteral("ok"))->textContent(),
             QStringLiteral("still ran"));

    bool reported = false;
    for (const javascript::ConsoleMessage &message : m_page->scripts()->messages()) {
        if (message.text.contains(QLatin1String("script failed on purpose")))
            reported = true;
    }
    QVERIFY(reported);
}

void ScriptingTest::collectsConsoleOutput()
{
    QVERIFY(m_directory.isValid());
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><script>console.log('from the page', 1 + 1);</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    bool found = false;
    for (const javascript::ConsoleMessage &message : m_page->scripts()->messages()) {
        if (message.text == QLatin1String("from the page 2"))
            found = true;
    }
    QVERIFY(found);
}

void ScriptingTest::finishesWhenTheOnlySubresourceIsAScript()
{
    QVERIFY(m_directory.isValid());
    QDir directory(m_directory.path());

    QFile script(directory.filePath(QStringLiteral("only.js")));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write("1 + 1;");
    script.close();

    // A page whose only subresource is a script is the case where a missed
    // completion signal leaves the page loading forever, which is what happened
    // before the accounting was fixed.
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><p>text</p><script src='only.js'></script></body></html>"),
                     directory));
    QVERIFY(!m_page->isLoading());
}

void ScriptingTest::firesDomContentLoaded()
{
    QVERIFY(m_directory.isValid());

    // A listener added by an inline script sees the event, and by the time it
    // runs the document is complete enough to query.
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='ready'>no</div>"
                         "<script>"
                         "document.addEventListener('DOMContentLoaded', function () {"
                         "  document.getElementById('ready').textContent ="
                         "    'ready:' + document.readyState;"
                         "});"
                         "</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    const QString text
        = m_page->document()->getElementById(QStringLiteral("ready"))->textContent();
    QVERIFY2(text.startsWith(QLatin1String("ready:")), qPrintable(text));
    // The state the listener saw is the one a browser reports at that point.
    QCOMPARE(text, QStringLiteral("ready:interactive"));
}

void ScriptingTest::firesLoadEvent()
{
    QVERIFY(m_directory.isValid());

    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='state'>no</div>"
                         "<script>"
                         "window.addEventListener('load', function () {"
                         "  document.getElementById('state').textContent = document.readyState;"
                         "});"
                         "</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("state"))->textContent(),
             QStringLiteral("complete"));
}

void ScriptingTest::runsTimersAndRelaysOut()
{
    QVERIFY(m_directory.isValid());

    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='tick'>waiting</div>"
                         "<script>"
                         "setTimeout(function () {"
                         "  document.getElementById('tick').textContent = 'ticked';"
                         "}, 10);"
                         "</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    // The timer has not run yet, since nothing has advanced the clock.
    QCOMPARE(m_page->document()->getElementById(QStringLiteral("tick"))->textContent(),
             QStringLiteral("waiting"));

    // Driving the clock forward runs it, and the document change reaches the
    // layout because serviceScripts rebuilds it.
    m_page->serviceScripts(1000);

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("tick"))->textContent(),
             QStringLiteral("ticked"));

    // The change reached the layout, so the tick is what would be painted.
    const QString painted = devtools::Inspector::boxTree(m_page->boxTree());
    QVERIFY2(painted.contains(QLatin1String("ticked")), qPrintable(painted));
}

void ScriptingTest::survivesAScriptThatRemovesItself()
{
    QVERIFY(m_directory.isValid());

    // A script element that removes itself, and the one after it, must not leave
    // the runner reading freed memory or running a script that is gone.
    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='r'>a</div>"
                         "<script>"
                         "document.getElementById('r').textContent += 'b';"
                         "var scripts = document.getElementsByTagName('script');"
                         "for (var i = scripts.length - 1; i >= 0; i--)"
                         "  scripts[i].parentNode.removeChild(scripts[i]);"
                         "document.getElementById('r').textContent += 'c';"
                         "</script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("r"))->textContent(),
             QStringLiteral("abc"));
}

void ScriptingTest::handlesAnInlineScriptWithNoContent()
{
    QVERIFY(m_directory.isValid());

    QVERIFY(loadPage(QStringLiteral(
                         "<html><body><div id='n'>fine</div>"
                         "<script></script>"
                         "<script>   </script>"
                         "<script><!-- --></script>"
                         "</body></html>"),
                     QDir(m_directory.path())));

    QCOMPARE(m_page->document()->getElementById(QStringLiteral("n"))->textContent(),
             QStringLiteral("fine"));
}

QTEST_MAIN(ScriptingTest)
#include "tst_scripting.moc"
