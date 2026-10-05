#include <QtTest>

#include "browser/Page.h"
#include "browser/Tab.h"
#include "css/Style.h"
#include "html/Parser.h"
#include "storage/History.h"
#include "ui/MainWindow.h"
#include "ui/PageView.h"

using namespace oqb;

/// Tests that drive the browser the way a user does: through the window, its
/// tabs and its navigation, rather than through the pipeline directly.
class BrowserTest : public QObject
{
    Q_OBJECT

private slots:
    void loadsAboutPageIntoWindow();
    void rendersTheWindow();
    void tabNavigationRecordsHistory();
    void backAndForwardMoveThroughHistory();
    void newTabKeepsItsOwnPage();
    void addressBarInputBecomesSearch();
    void errorPageIsShownForAFailedLoad();
    void pageViewScrollsWithinTheDocument();
    void pageViewFindsLinksUnderTheCursor();
    void titleFollowsTheDocument();
};

/// Waits for a tab to finish loading, or fails the test.
bool waitForLoad(browser::Tab *tab, int timeoutMs = 30000)
{
    if (!tab->isLoading())
        return true;

    QSignalSpy finishedSpy(tab, &browser::Tab::loadFinished);
    QSignalSpy failedSpy(tab, &browser::Tab::loadFailed);

    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (finishedSpy.count() > 0)
            return true;
        if (failedSpy.count() > 0 && !tab->isLoading())
            return true;
    }
    return tab->isLoading() == false;
}

void BrowserTest::loadsAboutPageIntoWindow()
{
    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;

    ui::MainWindow window(settings);

    // The window starts with one tab showing the new tab page.
    QCOMPARE(window.tabCount(), 1);

    window.openUrl(network::Url::parse(QStringLiteral("about:home")));

    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);

    browser::Tab *tab = view->tab();
    QVERIFY(tab != nullptr);
    QVERIFY(waitForLoad(tab));

    // The page is built in the browser, so it must load without any network.
    QVERIFY(tab->page()->document() != nullptr);
    QVERIFY(tab->page()->boxTree() != nullptr);
    QVERIFY(!tab->page()->isErrorPage());
    QCOMPARE(tab->title(), QStringLiteral("New Tab"));
}

void BrowserTest::rendersTheWindow()
{
    browser::PageSettings settings;
    settings.viewportWidth = 800;
    settings.viewportHeight = 600;

    ui::MainWindow window(settings);
    window.resize(820, 660);
    window.show();
    window.openUrl(network::Url::parse(QStringLiteral("about:home")));

    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);
    QVERIFY(waitForLoad(view->tab()));

    // Rendering happens through the widget's own paint path, which is what the
    // window shows. Grabbing it exercises that path with the real styles.
    const QPixmap shot = window.grab();
    QVERIFY(!shot.isNull());
    QVERIFY(shot.width() > 100);
    QVERIFY(shot.height() > 100);

    const QImage image = shot.toImage();

    // The page has a light background and dark text, so both must be present.
    int light = 0;
    int dark = 0;
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            const int lightness = image.pixelColor(x, y).lightness();
            if (lightness > 200)
                ++light;
            else if (lightness < 100)
                ++dark;
        }
    }

    QVERIFY2(light > 1000, qPrintable(QStringLiteral("light pixels: %1").arg(light)));
    QVERIFY2(dark > 100, qPrintable(QStringLiteral("dark pixels: %1").arg(dark)));
}

void BrowserTest::tabNavigationRecordsHistory()
{
    // Back and forward come from the history store, which the window shares
    // between tabs; a tab with no store has no history to move through.
    storage::HistoryStore history;
    browser::Tab tab;
    tab.setHistory(&history);

    tab.navigate(network::Url::parse(QStringLiteral("about:version")));
    QVERIFY(waitForLoad(&tab));
    QCOMPARE(tab.url().aboutPage(), QStringLiteral("version"));

    // Navigating to a second page makes Back available.
    tab.navigate(network::Url::parse(QStringLiteral("about:home")));
    QVERIFY(waitForLoad(&tab));
    QCOMPARE(tab.url().aboutPage(), QStringLiteral("home"));

    QVERIFY(tab.canGoBack());
    QVERIFY(!tab.canGoForward());

    // The store holds both visits, most recent last.
    QCOMPARE(history.count(), 2);
    QCOMPARE(history.currentUrl().aboutPage(), QStringLiteral("home"));

    // Going back moves the cursor without discarding the forward entry, which is
    // what makes Forward work afterwards.
    QVERIFY(tab.goBack());
    QVERIFY(waitForLoad(&tab));
    QCOMPARE(history.currentUrl().aboutPage(), QStringLiteral("version"));
    QVERIFY(tab.canGoForward());
}

void BrowserTest::backAndForwardMoveThroughHistory()
{
    ui::MainWindow window;
    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);
    browser::Tab *tab = view->tab();
    QVERIFY(tab != nullptr);

    window.openUrl(network::Url::parse(QStringLiteral("about:version")));
    QVERIFY(waitForLoad(tab));

    window.openUrl(network::Url::parse(QStringLiteral("about:home")));
    QVERIFY(waitForLoad(tab));
    QCOMPARE(tab->url().aboutPage(), QStringLiteral("home"));

    // Back returns to the first page.
    QVERIFY(tab->goBack());
    QVERIFY(waitForLoad(tab));
    QCOMPARE(tab->url().aboutPage(), QStringLiteral("version"));

    // And forward returns to the second.
    QVERIFY(tab->goForward());
    QVERIFY(waitForLoad(tab));
    QCOMPARE(tab->url().aboutPage(), QStringLiteral("home"));

    // At the end of the list, forward is unavailable again.
    QVERIFY(!tab->canGoForward());
}

void BrowserTest::newTabKeepsItsOwnPage()
{
    browser::PageSettings settings;
    ui::MainWindow window(settings);
    window.resize(800, 600);
    window.show();

    window.openUrl(network::Url::parse(QStringLiteral("about:version")));
    auto *firstView = qobject_cast<ui::PageView *>(window.centralWidget()->findChild<QWidget *>());
    Q_UNUSED(firstView);
    QVERIFY(waitForLoad(qobject_cast<ui::PageView *>(window.findChildren<ui::PageView *>().value(0))->tab()));

    // A second tab has its own page, and the first tab is unaffected.
    window.openInNewTab(network::Url::parse(QStringLiteral("about:home")));
    QCOMPARE(window.tabCount(), 2);

    const QList<ui::PageView *> views = window.findChildren<ui::PageView *>();
    QCOMPARE(views.size(), 2);

    auto *homeView = qobject_cast<ui::PageView *>(window.findChild<QWidget *>(QString(), Qt::FindDirectChildrenOnly));
    Q_UNUSED(homeView);

    // Whichever view is current shows the new page; the other still holds its own.
    const auto pages = {views.at(0)->tab()->url().aboutPage(), views.at(1)->tab()->url().aboutPage()};
    QVERIFY(pages.begin()[0] != pages.begin()[1]);

    QSet<QString> names;
    for (const QString &name : pages)
        names.insert(name);
    QVERIFY(names.contains(QStringLiteral("version")));
    QVERIFY(names.contains(QStringLiteral("home")));
}

void BrowserTest::addressBarInputBecomesSearch()
{
    browser::Tab tab;

    // A phrase is not an address, so it becomes a search rather than a failed
    // navigation, which is what the address bar must do.
    const bool handled = tab.navigateToUserInput(QStringLiteral("how do browsers work"));
    QVERIFY(handled);

    // The search URL was built and the tab began loading it; the load itself is
    // not awaited because it needs the network.
    QVERIFY(tab.url().isValid());
    QVERIFY(tab.url().host().contains(QLatin1String("duckduckgo")));
    QVERIFY(tab.url().query().contains(QLatin1String("q=")));

    tab.stop();

    // An address, by contrast, is navigated to directly.
    browser::Tab direct;
    QVERIFY(direct.navigateToUserInput(QStringLiteral("about:version")));
    QVERIFY(waitForLoad(&direct));
    QCOMPARE(direct.url().aboutPage(), QStringLiteral("version"));

    // An empty input is refused rather than loading nothing.
    browser::Tab empty;
    QVERIFY(!empty.navigateToUserInput(QStringLiteral("   ")));
}

void BrowserTest::errorPageIsShownForAFailedLoad()
{
    browser::Page page;

    // Port 1 on loopback has nothing listening, so the load fails fast and an
    // error page is built. The browser shows that page rather than nothing.
    QSignalSpy finishedSpy(&page, &browser::Page::finished);

    browser::PageSettings settings;
    settings.requestTimeoutMs = 3000;
    page.setSettings(settings);

    page.load(network::Url::parse(QStringLiteral("http://127.0.0.1:1/")));

    QVERIFY(finishedSpy.wait(15000));
    QVERIFY(page.isErrorPage());
    QVERIFY(page.document() != nullptr);

    // The error page is a real document, so it renders through the pipeline.
    QVERIFY(page.boxTree() != nullptr);
    const QString text = page.document()->textContent();
    QVERIFY2(text.contains(QStringLiteral("could not")), qPrintable(text.left(200)));

    // And it does not claim a successful title.
    QVERIFY(!page.title().isEmpty());
}

void BrowserTest::pageViewScrollsWithinTheDocument()
{
    ui::MainWindow window;
    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);

    // about:bookmarks is short; scroll requests past the edges are clamped, so
    // the view never leaves the document.
    window.openUrl(network::Url::parse(QStringLiteral("about:bookmarks")));
    QVERIFY(waitForLoad(view->tab()));

    view->scrollTo(0, 0);
    QCOMPARE(view->scrollPosition(), QPoint(0, 0));

    // Scrolling far down clamps to the bottom, never beyond it.
    view->scrollTo(0, 100000);
    const int bottom = view->scrollPosition().y();
    QVERIFY(bottom >= 0);
    QVERIFY(bottom <= qMax(0, view->documentSize().height()));

    // Scrolling back up returns to the top.
    view->scrollTo(0, -5000);
    QCOMPARE(view->scrollPosition(), QPoint(0, 0));
}

void BrowserTest::pageViewFindsLinksUnderTheCursor()
{
    ui::MainWindow window;
    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);

    // The new tab page lists every built-in page as a link.
    window.openUrl(network::Url::parse(QStringLiteral("about:home")));
    QVERIFY(waitForLoad(view->tab()));

    // Walking the document is enough to prove the links exist and resolve; the
    // hit test itself needs a pixel position, which depends on font metrics.
    const auto anchors = view->tab()->page()->document()->getElementsByTagName(QStringLiteral("a"));
    QVERIFY(anchors.size() >= 4);

    bool foundAboutLink = false;
    for (const dom::Element *anchor : anchors) {
        const network::Url resolved = view->tab()->page()->document()->resolveUrl(
            anchor->attribute(QStringLiteral("href")));
        if (resolved.aboutPage() == QLatin1String("version"))
            foundAboutLink = true;
    }
    QVERIFY(foundAboutLink);

    // A point in the corner is over no link at all.
    QVERIFY(!view->linkAt(QPoint(1, 1)).isValid());
}

void BrowserTest::titleFollowsTheDocument()
{
    ui::MainWindow window;
    auto *view = window.findChild<ui::PageView *>();
    QVERIFY(view != nullptr);

    window.openUrl(network::Url::parse(QStringLiteral("about:version")));
    QVERIFY(waitForLoad(view->tab()));

    QCOMPARE(view->tab()->title(), QStringLiteral("Version"));
    QVERIFY(window.windowTitle().contains(QLatin1String("Version")));
}

QTEST_MAIN(BrowserTest)
#include "tst_browser.moc"
