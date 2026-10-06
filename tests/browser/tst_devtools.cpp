#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <QTcpServer>
#include <QTcpSocket>

#include "browser/Tab.h"
#include "dom/Document.h"
#include "storage/Cookies.h"
#include "ui/DevToolsPanel.h"
#include "ui/MainWindow.h"
#include "ui/PageView.h"

using namespace oqb;

/// A loopback server that answers every request with the same body and a cookie.
///
/// The panel's cookie view distinguishes cookies that apply to the page on
/// screen from ones that do not, and only a real http:// page has a host that can
/// receive one. A file:// document cannot, so the positive case needs a socket.
class LoopbackServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit LoopbackServer(const QByteArray &body, QObject *parent = nullptr)
        : QTcpServer(parent)
        , m_body(body)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    socket->readAll();
                    QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                                          "Set-Cookie: fromserver=1; Path=/\r\n"
                                          "Content-Length: ";
                    response += QByteArray::number(m_body.size());
                    response += "\r\nConnection: close\r\n\r\n";
                    response += m_body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool start() { return listen(QHostAddress::LocalHost, 0); }

    network::Url urlFor(const QString &path) const
    {
        return network::Url::parse(
            QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path));
    }

private:
    QByteArray m_body;
};

/// Tests for the developer tools panel.
///
/// The panel is driven the way a user drives it: through a real window, a real
/// load and the widgets themselves. That is the only way to catch the failures
/// that matter here, which are not crashes but a view that quietly shows the
/// wrong thing - a stale tree, a console line that never appears, or an
/// expression that runs in the wrong document.
class DevToolsTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void showsTheDocumentTree();
    void selectsAnElementAndShowsItsStyle();
    void evaluatesExpressionsInThePage();
    void reportsEvaluationErrors();
    void evaluatesAgainstThePageDom();
    void showsScriptsConsoleOutput();
    void showsTheLayoutSummary();
    void listsRequestedResources();
    void followsTheTabAndClearsOnNavigation();
    void picksAnElementFromThePage();
    void survivesAnElementRemovedAfterSelection();
    void showsTheCookieJar();
    void marksCookiesThatDoNotApplyToThePage();

private:
    /// Loads `html` in a tab of its own and returns the panel.
    ui::DevToolsPanel *loadPage(const QString &html);
    ui::DevToolsPanel *loadPage(const QString &html, const QString &fileName);

    /// The tab the window is currently showing.
    browser::Tab *currentTab() const;
    /// The view showing `tab`.
    ui::PageView *viewForTab(browser::Tab *tab) const;

    /// Waits for the tab to finish loading.
    bool waitForLoad(browser::Tab *tab, int timeoutMs = 15000);

    QTemporaryDir m_directory;
    std::unique_ptr<ui::MainWindow> m_window;
    ui::DevToolsPanel *m_panel = nullptr;
};

void DevToolsTest::init()
{
    QVERIFY(m_directory.isValid());

    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;

    m_window = std::make_unique<ui::MainWindow>(settings);
    m_panel = m_window->findChild<ui::DevToolsPanel *>();
    QVERIFY(m_panel != nullptr);
}

void DevToolsTest::cleanup()
{
    m_panel = nullptr;
    m_window.reset();
}

bool DevToolsTest::waitForLoad(browser::Tab *tab, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (tab->isLoading() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !tab->isLoading();
}

ui::DevToolsPanel *DevToolsTest::loadPage(const QString &html)
{
    return loadPage(html, QStringLiteral("page.html"));
}

ui::DevToolsPanel *DevToolsTest::loadPage(const QString &html, const QString &fileName)
{
    QFile file(QDir(m_directory.path()).filePath(fileName));
    if (!file.open(QIODevice::WriteOnly))
        return nullptr;
    file.write(html.toUtf8());
    file.close();

    // The window opens with a tab showing about:home, so the page is loaded in a
    // tab of its own and that tab is the one driven. Addressing whichever view
    // findChild() returns first would drive the welcome page instead.
    m_window->openInNewTab(network::Url::fromLocalFile(file.fileName()));

    auto *view = m_window->findChild<ui::PageView *>();
    Q_UNUSED(view);

    browser::Tab *tab = currentTab();
    if (!tab || !waitForLoad(tab))
        return nullptr;

    m_panel->setTab(tab);
    m_panel->setPageView(viewForTab(tab));
    m_panel->refresh();

    return m_panel;
}

browser::Tab *DevToolsTest::currentTab() const
{
    // The view the window is showing, which is the one the panel is attached to.
    auto *tabs = m_window->findChild<QTabWidget *>();
    if (!tabs)
        return nullptr;
    auto *view = qobject_cast<ui::PageView *>(tabs->currentWidget());
    return view ? view->tab() : nullptr;
}

ui::PageView *DevToolsTest::viewForTab(browser::Tab *tab) const
{
    for (ui::PageView *view : m_window->findChildren<ui::PageView *>()) {
        if (view->tab() == tab)
            return view;
    }
    return nullptr;
}

void DevToolsTest::showsTheDocumentTree()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><div id='wrap' class='outer'><p>hello</p><span>world</span></div>"
        "</body></html>"));
    QVERIFY(panel != nullptr);

    auto *tree = panel->findChild<QTreeWidget *>(QStringLiteral("elementTree"));
    QVERIFY(tree != nullptr);
    QCOMPARE(tree->topLevelItemCount(), 1);

    // The root is the html element, and a reader can find the wrapper by its id
    // and class, which is what makes a tree of divs legible.
    QTreeWidgetItem *root = tree->topLevelItem(0);
    QCOMPARE(root->text(0), QStringLiteral("html"));

    // A text-only element is shown with its text quoted.
    bool foundText = false;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        if (item->text(0).contains(QLatin1String("\"hello\"")))
            foundText = true;
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    walk(root);
    QVERIFY2(foundText, "the paragraph's text should appear in the tree");

    // The id and class are part of the label.
    bool foundWrap = false;
    walk = [&](QTreeWidgetItem *item) {
        if (item->text(0) == QLatin1String("div#wrap.outer"))
            foundWrap = true;
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    walk(root);
    QVERIFY2(foundWrap, "an element with an id and a class should show both");
}

void DevToolsTest::selectsAnElementAndShowsItsStyle()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><head><style>#box { color: rgb(255, 0, 0); width: 120px; }</style></head>"
        "<body><div id='box'>styled</div></body></html>"));
    QVERIFY(panel != nullptr);

    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    dom::Element *box = tab->page()->document()->getElementById(QStringLiteral("box"));
    QVERIFY(box != nullptr);

    panel->revealElement(box);

    auto *tree = panel->findChild<QTreeWidget *>(QStringLiteral("elementTree"));
    QVERIFY(tree->currentItem() != nullptr);
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("div#box"));

    // The computed style view shows the declarations the cascade produced.
    auto *styleView = panel->findChild<QPlainTextEdit *>(QStringLiteral("computedStyle"));
    QVERIFY(styleView != nullptr);
    const QString style = styleView->toPlainText();
    QVERIFY2(style.contains(QLatin1String("rgb(255, 0, 0)"))
                 || style.contains(QLatin1String("#ff0000")),
             qPrintable(style));
    QVERIFY2(style.contains(QLatin1String("120px")), qPrintable(style));

    // And the rules view says which selector set them.
    auto *rulesView = panel->findChild<QPlainTextEdit *>(QStringLiteral("appliedRules"));
    QVERIFY(rulesView != nullptr);
    QVERIFY2(rulesView->toPlainText().contains(QLatin1String("#box")),
             qPrintable(rulesView->toPlainText()));
}

void DevToolsTest::evaluatesExpressionsInThePage()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><div id='out'>unchanged</div></body></html>"));
    QVERIFY(panel != nullptr);

    auto *input = panel->findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    QVERIFY(input != nullptr);

    // An expression typed into the console runs in the page, and its value is
    // echoed back the way a console does.
    input->setText(QStringLiteral("2 + 3"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();

    auto *console = panel->findChild<QPlainTextEdit *>(QStringLiteral("consoleOutput"));
    QVERIFY(console != nullptr);
    QVERIFY2(console->toPlainText().contains(QLatin1String("2 + 3")),
             qPrintable(console->toPlainText()));
    QVERIFY2(console->toPlainText().contains(QLatin1String("5")),
             qPrintable(console->toPlainText()));
}

void DevToolsTest::reportsEvaluationErrors()
{
    auto *panel = loadPage(QStringLiteral("<html><body></body></html>"));
    QVERIFY(panel != nullptr);

    auto *input = panel->findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    input->setText(QStringLiteral("noSuchFunction()"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();

    auto *console = panel->findChild<QPlainTextEdit *>(QStringLiteral("consoleOutput"));
    QVERIFY(console != nullptr);
    // The failure is reported rather than swallowed, and the panel is still
    // usable afterwards.
    QVERIFY2(console->toPlainText().contains(QLatin1String("noSuchFunction")),
             qPrintable(console->toPlainText()));

    input->setText(QStringLiteral("1 + 1"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();
    QVERIFY(console->toPlainText().contains(QLatin1String("2")));
}

void DevToolsTest::evaluatesAgainstThePageDom()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><div id='target'>original</div></body></html>"));
    QVERIFY(panel != nullptr);

    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    browser::Page *page = tab->page();

    // The console sees the page's own document, which is what makes it useful:
    // a change made here has to land in the DOM the browser is showing.
    auto *input = panel->findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    input->setText(QStringLiteral("document.getElementById('target').textContent = 'changed'"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();

    QCOMPARE(page->document()->getElementById(QStringLiteral("target"))->textContent(),
             QStringLiteral("changed"));

    // And it can read the document back.
    input->setText(QStringLiteral("document.getElementById('target').textContent"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();

    auto *console = panel->findChild<QPlainTextEdit *>(QStringLiteral("consoleOutput"));
    QVERIFY2(console->toPlainText().contains(QLatin1String("changed")),
             qPrintable(console->toPlainText()));
}

void DevToolsTest::showsScriptsConsoleOutput()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><script>console.log('page says hello');"
        "console.warn('and a warning');</script></body></html>"));
    QVERIFY(panel != nullptr);

    auto *console = panel->findChild<QPlainTextEdit *>(QStringLiteral("consoleOutput"));
    QVERIFY(console != nullptr);
    QVERIFY2(console->toPlainText().contains(QLatin1String("page says hello")),
             qPrintable(console->toPlainText()));
    QVERIFY2(console->toPlainText().contains(QLatin1String("and a warning")),
             qPrintable(console->toPlainText()));
}

void DevToolsTest::showsTheLayoutSummary()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body style='margin:0'><div style='height:300px'>tall</div></body></html>"));
    QVERIFY(panel != nullptr);

    // The layout tab reports the document size from the real layout result.
    auto *layoutView = panel->findChild<QPlainTextEdit *>(QStringLiteral("layoutReport"));
    QVERIFY(layoutView != nullptr);
    const QString report = layoutView->toPlainText();
    QVERIFY2(report.contains(QLatin1String("document:")), qPrintable(report));
    // The div is 300px tall, so the document is at least that.
    QVERIFY2(report.contains(QLatin1String("300")), qPrintable(report));
}

void DevToolsTest::listsRequestedResources()
{
    QDir directory(m_directory.path());

    // A stylesheet reference is enough to produce a request, and the file does
    // not even have to exist for the request to be listed.
    QFile css(directory.filePath(QStringLiteral("style.css")));
    QVERIFY(css.open(QIODevice::WriteOnly));
    css.write("body { color: rgb(0, 0, 255); }");
    css.close();

    auto *panel = loadPage(QStringLiteral(
        "<html><head><link rel='stylesheet' href='style.css'></head>"
        "<body><p>styled</p></body></html>"));
    QVERIFY(panel != nullptr);

    auto *networkView = panel->findChild<QPlainTextEdit *>(QStringLiteral("resourceReport"));
    QVERIFY(networkView != nullptr);
    QVERIFY2(networkView->toPlainText().contains(QLatin1String("style.css")),
             qPrintable(networkView->toPlainText()));
}

void DevToolsTest::followsTheTabAndClearsOnNavigation()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><div id='first'>one</div><script>console.log('first page');</script>"
        "</body></html>"));
    QVERIFY(panel != nullptr);

    auto *console = panel->findChild<QPlainTextEdit *>(QStringLiteral("consoleOutput"));
    QVERIFY(console->toPlainText().contains(QLatin1String("first page")));

    auto *tree = panel->findChild<QTreeWidget *>(QStringLiteral("elementTree"));

    // A navigation replaces the document, so the console must not keep the
    // previous page's output and the tree must be built from the new document.
    // The lookup is by lambda so it always reads the widgets' current state
    // rather than a pointer captured before the load.
    const auto treeContains = [this](const QString &needle) {
        auto *view = m_window->findChild<QTreeWidget *>(QStringLiteral("elementTree"));
        if (!view || view->topLevelItemCount() == 0)
            return false;

        bool found = false;
        std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
            if (item->text(0).contains(needle))
                found = true;
            for (int i = 0; i < item->childCount(); ++i)
                walk(item->child(i));
        };
        for (int i = 0; i < view->topLevelItemCount(); ++i)
            walk(view->topLevelItem(i));
        return found;
    };

    QFile second(QDir(m_directory.path()).filePath(QStringLiteral("second.html")));
    QVERIFY(second.open(QIODevice::WriteOnly));
    second.write("<html><body><div id='second'>two</div></body></html>");
    second.close();

    m_window->openUrl(network::Url::fromLocalFile(second.fileName()));
    QVERIFY(waitForLoad(currentTab()));

    // The window queues its own refresh when the load finishes, so the view is
    // given the chance to catch up before it is inspected.
    QElapsedTimer settle;
    settle.start();
    while (!treeContains(QStringLiteral("#second")) && settle.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    panel->refresh();

    // The new document's element is in the tree, and the previous page's output
    // is gone from the console.
    QVERIFY2(treeContains(QStringLiteral("#second")),
             "the new document's element should be in the tree");
    QVERIFY2(!treeContains(QStringLiteral("#first")),
             "the previous document's element should be gone");
    QVERIFY2(!console->toPlainText().contains(QLatin1String("first page")),
             qPrintable(console->toPlainText()));
}

void DevToolsTest::picksAnElementFromThePage()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body style='margin:0'>"
        "<div id='big' style='width:400px; height:200px'></div>"
        "</body></html>"));
    QVERIFY(panel != nullptr);

    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    ui::PageView *view = viewForTab(tab);
    QVERIFY(view != nullptr);

    // Picking a point in the page has to find the element that generated the box
    // there, which is what the picker relies on.
    const dom::Element *picked = view->elementAtPoint(QPoint(50, 50));
    QVERIFY(picked != nullptr);
    QCOMPARE(picked->id(), QStringLiteral("big"));

    // The picker mode is exposed and reverts on its own when a click lands, so
    // it cannot be left armed by accident.
    QVERIFY(!view->isPickingElement());
    view->setPickingElement(true);
    QVERIFY(view->isPickingElement());
    view->setPickingElement(false);
    QVERIFY(!view->isPickingElement());
}

void DevToolsTest::survivesAnElementRemovedAfterSelection()
{
    auto *panel = loadPage(QStringLiteral(
        "<html><body><div id='gone'>here</div><div id='stays'>and here</div></body></html>"));
    QVERIFY(panel != nullptr);

    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    browser::Page *page = tab->page();

    dom::Element *target = page->document()->getElementById(QStringLiteral("gone"));
    QVERIFY(target != nullptr);
    panel->revealElement(target);

    auto *tree = panel->findChild<QTreeWidget *>(QStringLiteral("elementTree"));
    QVERIFY(tree->currentItem() != nullptr);

    // A script removes the element the panel is showing. Refreshing must not
    // crash on the dangling pointer, and must leave the views consistent.
    auto *input = panel->findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    input->setText(QStringLiteral("document.getElementById('gone').remove()"));
    QMetaObject::invokeMethod(input, "returnPressed");
    QCoreApplication::processEvents();

    panel->refresh();

    // The tree no longer contains it, and either nothing is selected or the
    // selection moved - both are acceptable; showing a stale element is not.
    QCOMPARE(page->document()->getElementById(QStringLiteral("gone")),
             static_cast<dom::Element *>(nullptr));
    if (tree->currentItem() != nullptr)
        QVERIFY(!tree->currentItem()->text(0).contains(QLatin1String("#gone")));
}

QTEST_MAIN(DevToolsTest)
#include "tst_devtools.moc"

void DevToolsTest::showsTheCookieJar()
{
    auto *panel = loadPage(QStringLiteral("<html><body>cookies</body></html>"));
    QVERIFY(panel != nullptr);

    auto *jarView = panel->findChild<QPlainTextEdit *>(QStringLiteral("cookieJar"));
    QVERIFY(jarView != nullptr);

    // A file:// page can neither set nor receive a cookie, so the jar the window
    // owns is reached through the tab's page and filled directly. The view is
    // about the jar's contents, and those are the same however they arrived.
    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    storage::CookieJar *jar = tab->page()->cookieJar();
    QVERIFY(jar != nullptr);
    QVERIFY(jar->isEmpty());

    // An empty jar says so rather than showing a bare header.
    QVERIFY2(jarView->toPlainText().contains(QLatin1String("No cookies stored")),
             qPrintable(jarView->toPlainText()));

    jar->store(QStringLiteral("session=abc; Path=/; HttpOnly; SameSite=Lax"),
               network::Url::parse(QStringLiteral("https://example.com/")));
    QCOMPARE(jar->count(), 1);

    panel->refresh();
    const QString text = jarView->toPlainText();
    QVERIFY2(text.contains(QLatin1String("session=abc")), qPrintable(text));
    // The traits are what a user opens the tab to read, so they are shown.
    QVERIFY2(text.contains(QLatin1String("HttpOnly")), qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("SameSite=Lax")), qPrintable(text));
    QVERIFY2(text.contains(QLatin1String("example.com")), qPrintable(text));
}

void DevToolsTest::marksCookiesThatDoNotApplyToThePage()
{
    auto *server = new LoopbackServer("<html><body>scope</body></html>", this);
    QVERIFY(server->start());

    m_window->openInNewTab(server->urlFor(QStringLiteral("/")));
    browser::Tab *tab = currentTab();
    QVERIFY(tab != nullptr);
    QVERIFY(waitForLoad(tab));
    m_panel->setTab(tab);
    m_panel->setPageView(viewForTab(tab));
    m_panel->refresh();

    storage::CookieJar *jar = tab->page()->cookieJar();
    QVERIFY(jar != nullptr);

    // The server set a cookie for the page's own host, so that one applies. A
    // second cookie is added for an unrelated host, which is the case the view
    // has to distinguish - a jar belongs to the window, so it holds cookies the
    // page on screen will never send.
    QVERIFY2(jar->count() >= 1, "the loopback server's cookie was not stored");
    jar->store(QStringLiteral("theirs=1; Path=/"),
               network::Url::parse(QStringLiteral("https://tracker.example/")));

    m_panel->refresh();

    auto *jarView = m_panel->findChild<QPlainTextEdit *>(QStringLiteral("cookieJar"));
    QVERIFY(jarView != nullptr);

    QString applying;
    QString notApplying;
    for (const QString &line : jarView->toPlainText().split(u'\n')) {
        if (line.contains(QLatin1String("fromserver=1")))
            applying = line;
        if (line.contains(QLatin1String("theirs=1")))
            notApplying = line;
    }

    QVERIFY2(!applying.isEmpty(), qPrintable(jarView->toPlainText()));
    QVERIFY2(!notApplying.isEmpty(), qPrintable(jarView->toPlainText()));

    // The cookie for the page's own host is sent, so it is not flagged; the one
    // for another host is not, so it is. Both branches are exercised, which a
    // test that only ever saw "!" could not tell from a hardcoded marker.
    QVERIFY2(!applying.startsWith(QLatin1String("!")), qPrintable(applying));
    QVERIFY2(notApplying.startsWith(QLatin1String("!")), qPrintable(notApplying));
}
