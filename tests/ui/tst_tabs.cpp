#include <QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebEngineHistory>
#include <QWebEngineUrlScheme>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include "ui/MainWindow.h"
#include "ui/TabBar.h"
#include "ui/WebTab.h"

using namespace oqb;

/// Runs a nested event loop until `predicate` holds or the deadline passes.
static bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 15000)
{
    if (predicate())
        return true;

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate())
            loop.quit();
    });

    poll.start();
    deadline.start(timeoutMs);
    loop.exec();
    return predicate();
}

/// The window's tabs, driven the way a user drives them.
///
/// The tests are about tab bookkeeping - how many exist, which one is in front,
/// and which of the many ways to open or close one work - because that is where
/// the window's own logic lives. Chromium renders the pages themselves.
class TabsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void startsWithOneTab();
    void newTabButtonOpensATab();
    void doubleClickOnEmptyStripOpensATab();
    void windowOpenCreatesATab();
    void eachTabHasItsOwnView();
    void closingATabRemovesIt();
    void closingTheLastTabOpensAFreshOne();
    void switchingTabsShowsThatTab();
    void shortcutOpensATab();
    void tabShowsThePageIcon();
    void backAndForwardWalkTheHistory();
    /// Writes a PNG of the window so the chrome can be reviewed by eye.
    ///
    /// It is skipped unless OQB_UI_PREVIEW is set to the file to write: a
    /// screenshot is not an assertion, and a test that always writes files
    /// would litter the build directory.
    void writePreview();

private:
    static QTabBar *tabBar(ui::MainWindow *window) { return window->findChild<ui::TabBar *>(); }
    static QStackedWidget *stack(ui::MainWindow *window)
    {
        return window->findChild<QStackedWidget *>();
    }
    /// The window's "+" button, found by the name the theme gives it.
    static QToolButton *newTabButton(ui::MainWindow *window)
    {
        return window->findChild<QToolButton *>(QStringLiteral("NewTabButton"));
    }
    /// True while the tab in front is still loading, for the preview.
    static bool currentLoading(ui::MainWindow &window)
    {
        const QList<ui::WebTab *> tabs = window.findChildren<ui::WebTab *>();
        return !tabs.isEmpty() && tabs.last()->isLoading();
    }
    /// The toolbar's back and forward buttons, found by the names the theme
    /// gives them.
    static QToolButton *navButton(ui::MainWindow *window, const QString &name)
    {
        return window->findChild<QToolButton *>(name);
    }
    /// The address bar, found by the name the theme gives it.
    static QLineEdit *windowAddressBar(ui::MainWindow *window)
    {
        return window->findChild<QLineEdit *>(QStringLiteral("AddressBar"));
    }
};

void TabsTest::initTestCase()
{
    // Keep the tests off the user's real profile, so nothing is written to the
    // developer's home directory and runs do not affect each other.
    QWebEngineProfile *profile = QWebEngineProfile::defaultProfile();
    profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
}

void TabsTest::startsWithOneTab()
{
    ui::MainWindow window;
    QVERIFY(tabBar(&window));
    QCOMPARE(tabBar(&window)->count(), 1);
    QCOMPARE(window.tabCount(), 1);
}

void TabsTest::newTabButtonOpensATab()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    QToolButton *plus = newTabButton(&window);
    QVERIFY2(plus, "the tab strip must show a way to open a new tab");

    const int before = tabs->count();
    plus->click();

    QCOMPARE(tabs->count(), before + 1);
    QCOMPARE(tabs->currentIndex(), tabs->count() - 1);
}

void TabsTest::doubleClickOnEmptyStripOpensATab()
{
    ui::MainWindow window;
    window.show();
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    const int before = tabs->count();

    // A point past the last tab is empty strip space, which is where the
    // gesture applies.
    const QPoint emptyPoint(tabs->width() - 4, tabs->height() / 2);
    QTest::mouseDClick(tabs, Qt::LeftButton, Qt::NoModifier, emptyPoint);

    QCOMPARE(tabs->count(), before + 1);
}

void TabsTest::windowOpenCreatesATab()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    ui::WebTab *tab = window.findChild<ui::WebTab *>();
    QVERIFY(tab);

    // window.open() is the path a popup and a target=_blank link take, so this
    // exercises Chromium asking for a window, the view creating one and the
    // window adopting it as a tab.
    const int before = tabs->count();
    tab->page()->setHtml(QStringLiteral(
        "<html><body><script>window.open('about:blank');</script></body></html>"));

    QVERIFY2(waitFor([&] { return tabs->count() > before; }),
             "window.open() did not create a tab");
}

void TabsTest::eachTabHasItsOwnView()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    window.openInNewTab(network::Url::parse(QStringLiteral("about:version")));
    window.openInNewTab(network::Url::parse(QStringLiteral("about:bookmarks")));
    QCOMPARE(tabs->count(), 3);

    const QList<ui::WebTab *> webTabs = window.findChildren<ui::WebTab *>();
    QCOMPARE(webTabs.size(), 3);

    // A shared view would mean the pages overwrite each other, which is the
    // failure that makes a second tab look empty.
    QSet<QWebEngineView *> views;
    QSet<QWebEnginePage *> pages;
    for (ui::WebTab *tab : webTabs) {
        views.insert(tab->view());
        pages.insert(tab->page());
    }
    QCOMPARE(views.size(), 3);
    QCOMPARE(pages.size(), 3);
}

void TabsTest::closingATabRemovesIt()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    window.openInNewTab(network::Url::parse(QStringLiteral("about:home")));
    QCOMPARE(tabs->count(), 2);

    // The close button the strip draws is what the user clicks, so the test
    // clicks it rather than emitting the signal behind it.
    auto *close = qobject_cast<QToolButton *>(tabs->tabButton(1, QTabBar::RightSide));
    QVERIFY2(close, "every tab must carry a close button");
    close->click();

    QCOMPARE(tabs->count(), 1);
}

void TabsTest::closingTheLastTabOpensAFreshOne()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    // Close the only tab; a browser never sits empty.
    tabs->tabButton(0, QTabBar::RightSide);
    emit static_cast<ui::TabBar *>(tabs)->closeRequested(0);

    QCOMPARE(tabs->count(), 1);
}

void TabsTest::switchingTabsShowsThatTab()
{
    ui::MainWindow window;
    QTabBar *tabs = tabBar(&window);
    QStackedWidget *pages = stack(&window);
    QVERIFY(tabs && pages);

    window.openInNewTab(network::Url::parse(QStringLiteral("about:version")));
    QCOMPARE(pages->count(), 2);

    tabs->setCurrentIndex(0);
    QCOMPARE(pages->currentIndex(), 0);

    tabs->setCurrentIndex(1);
    QCOMPARE(pages->currentIndex(), 1);

    // The page in front is the tab that says it is.
    QCOMPARE(pages->currentWidget(), static_cast<QWidget *>(window.findChildren<ui::WebTab *>()
                                                               .value(tabs->currentIndex())));
}

void TabsTest::shortcutOpensATab()
{
    ui::MainWindow window;
    window.show();
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    const int before = tabs->count();

    // Asked for by its platform-independent name, so the test does not hard-code
    // Control where macOS uses Command.
    const QKeySequence addTab = QKeySequence(QKeySequence::AddTab);
    QVERIFY(!addTab.isEmpty());

    // Give the page focus first: that is the state the user is in after a page
    // loads, and it is where a window-context shortcut would stop working,
    // because WebEngine renders into its own widget and consumes the key event.
    ui::WebTab *tab = window.findChild<ui::WebTab *>();
    QVERIFY(tab);
    tab->view()->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(tab->view()->hasFocus(), 5000);

    QTest::keySequence(&window, addTab);
    QVERIFY2(waitFor([&] { return tabs->count() > before; }),
             "the New Tab shortcut did not open a tab");

    // And again with the key delivered to the widget that has focus, which is
    // where the user's keystrokes actually arrive.
    const int afterFirst = tabs->count();
    QTest::keySequence(QApplication::focusWidget() ? QApplication::focusWidget() : &window,
                       addTab);
    QVERIFY2(waitFor([&] { return tabs->count() > afterFirst; }),
             "the shortcut did not fire while the page had focus");
}

void TabsTest::tabShowsThePageIcon()
{
    // A 16x16 PNG, kept inline so the test needs no fixture file.
    static const QByteArray kFaviconPng = QByteArrayLiteral(
        "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAIElEQVR42mP4jwPIltwgCjOMGkAFA4hV"
        "OGrAqAE0NQAAQRKg77STjB8AAAAASUVORK5CYII=");

    ui::MainWindow window;
    window.show();
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);

    ui::WebTab *tab = window.findChild<ui::WebTab *>();
    QVERIFY(tab);

    // A data: icon is used so the test does not depend on a site's favicon.
    // A local server is used rather than a data: URL or a real site: Chromium
    // fetches a favicon the way it fetches any subresource, and a real site in
    // an offscreen test loses its GPU context.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                const QByteArray request = socket->readAll();
                QByteArray body;
                QByteArray type;
                if (request.startsWith("GET /favicon.png")) {
                    body = QByteArray::fromBase64(kFaviconPng);
                    type = "image/png";
                } else {
                    body = QByteArray("<html><head><link rel=\"icon\" href=\"/favicon.png\">"
                                      "</head><body>icon</body></html>");
                    type = "text/html";
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type
                              + "\r\nContent-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });

    tab->page()->setUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())));

    // The tab reflects the page's icon, which is how a user tells tabs apart.
    QVERIFY2(waitFor([&] { return !tabs->tabIcon(0).isNull(); }, 6000),
             "the tab did not pick up the page icon");
}

void TabsTest::writePreview()
{
    const QString target = qEnvironmentVariable("OQB_UI_PREVIEW");
    if (target.isEmpty())
        QSKIP("set OQB_UI_PREVIEW=/path/to/preview.png to write a screenshot");

    ui::MainWindow window;
    window.resize(1280, 820);
    window.show();

    // Two tabs, so the strip shows both the selected and the idle state.
    window.openInNewTab(network::Url::parse(QStringLiteral("about:version")));
    QTabBar *tabs = tabBar(&window);
    QVERIFY(tabs);
    QCOMPARE(tabs->count(), 2);

    // A page can be named so the preview shows the chrome over real content.
    const QString pageUrl = qEnvironmentVariable("OQB_UI_PREVIEW_URL");
    if (!pageUrl.isEmpty()) {
        window.openInNewTab(network::Url::parse(pageUrl));
        QCOMPARE(tabs->count(), 3);
        // Give the network load time to finish before the grab.
        waitFor([&] { return !currentLoading(window); }, 20000);
    }

    // Let the pages lay out, fetch their icons and paint before the grab.
    QTest::qWait(4000);

    const QPixmap shot = window.grab();
    QVERIFY2(!shot.isNull(), "the window produced no image");
    QVERIFY2(shot.save(target), qPrintable(QStringLiteral("cannot write ") + target));
}

void TabsTest::backAndForwardWalkTheHistory()
{
    // A local server, so the test does not depend on the network. It serves a
    // page that names its own address, which is how the test tells which one it
    // is looking at.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                const QByteArray request = socket->readAll();
                const QByteArray path = request.mid(4, request.indexOf(' ', 4) - 4);
                const QByteArray body =
                    "<html><head><title>" + path + "</title></head><body>" + path
                    + "</body></html>";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n"
                              + body);
                socket->disconnectFromHost();
            });
        }
    });

    const QString first = QStringLiteral("http://127.0.0.1:%1/first").arg(server.serverPort());
    const QString second = QStringLiteral("http://127.0.0.1:%1/second").arg(server.serverPort());

    ui::MainWindow window;
    window.show();

    QToolButton *back = navButton(&window, QStringLiteral("BackButton"));
    QToolButton *forward = navButton(&window, QStringLiteral("ForwardButton"));
    QVERIFY2(back, "the toolbar must have a back button");
    QVERIFY2(forward, "the toolbar must have a forward button");

    ui::WebTab *tab = window.findChild<ui::WebTab *>();
    QVERIFY(tab);

    // The start page has to be somewhere the user can come back to, or Back is
    // dead the first time a site is opened.
    // The start page has to be a real navigation, or Back is dead the first
    // time a site is opened.
    QSignalSpy startDone(tab, &ui::WebTab::loadFinished);
    QVERIFY2(waitFor([&] { return startDone.count() > 0; }, 10000),
             "the start page did not load");
    QCOMPARE(tab->view()->history()->count(), 1);

    // Two pages in the same tab, so there is somewhere to go back to. The waits
    // are on the load signals rather than on the tab's own state, which is set
    // synchronously by navigate() and would let the test race ahead.
    const auto loadAndWait = [&](const QString &url) {
        QSignalSpy finished(tab, &ui::WebTab::loadFinished);
        window.openUrl(network::Url::parse(url));
        if (!waitFor([&] { return finished.count() > 0; }, 15000))
            return false;
        return waitFor([&] { return !tab->isLoading(); }, 5000);
    };

    QVERIFY2(loadAndWait(first), "the first page did not load");
    QCOMPARE(tab->url().toString(), first);
    QVERIFY2(loadAndWait(second), "the second page did not load");
    QCOMPARE(tab->url().toString(), second);

    // With two entries, back is available and forward is not.
    QVERIFY2(waitFor([&] { return back->isEnabled(); }, 5000),
             "back must be enabled after visiting a second page");
    QVERIFY2(!forward->isEnabled(), "forward must be disabled at the newest page");

    // Clicking the toolbar button goes to the previous page.
    back->click();
    QVERIFY2(waitFor([&] { return tab->url().toString() == first; }, 15000),
             "the back button did not return to the previous page");
    QVERIFY(waitFor([&] { return !tab->isLoading(); }, 15000));

    // And forward returns to the one just left.
    QVERIFY2(waitFor([&] { return forward->isEnabled(); }, 5000),
             "forward must be enabled after going back");
    forward->click();
    QVERIFY2(waitFor([&] { return tab->url().toString() == second; }, 15000),
             "the forward button did not return to the next page");

    // The address bar follows, which is how the user sees where they are.
    QTRY_COMPARE_WITH_TIMEOUT(windowAddressBar(&window)->text(), second, 5000);
}

QTEST_MAIN(TabsTest)
#include "tst_tabs.moc"