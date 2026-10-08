#include "ui/WebTab.h"

#include "browser/BuiltinPages.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"

#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

namespace oqb::ui {

namespace {

/// A QWebEngineView that asks the window for a view when a page opens a popup
/// or a target=_blank link. The view it returns is adopted into a new tab,
/// which is what a browser does with a link that asks for a new window.
class WebView : public QWebEngineView
{
    Q_OBJECT

public:
    explicit WebView(QWebEngineProfile *profile, QWidget *parent = nullptr)
        : QWebEngineView(parent)
        , m_profile(profile)
    {
    }

protected:
    QWebEngineView *createWindow(QWebEnginePage::WebWindowType type) override
    {
        Q_UNUSED(type);
        // The new view shares the window's profile, so its cookies and cache
        // are the same and a login opened in a popup is still logged in.
        auto *view = new WebView(m_profile);
        emit newViewRequested(view);
        return view;
    }

signals:
    void newViewRequested(QWebEngineView *view);

private:
    QWebEngineProfile *m_profile = nullptr;
};

} // namespace

WebTab::WebTab(QWebEngineProfile *profile, QWidget *parent)
    : QWidget(parent)
    , m_profile(profile)
{
    m_view = new WebView(m_profile, this);
    m_page = m_view->page();
    connectView();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

WebTab::WebTab(QWebEngineView *view, QWebEngineProfile *profile, QWidget *parent)
    : QWidget(parent)
    , m_view(view)
    , m_profile(profile)
{
    m_view->setParent(this);
    m_page = m_view->page();
    connectView();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

WebTab::~WebTab() = default;

void WebTab::connectView()
{
    if (!m_view || !m_page)
        return;

    m_page->setBackgroundColor(Qt::white);

    connect(m_view, &QWebEngineView::titleChanged, this, [this](const QString &title) {
        m_title = title;
        emit titleChanged(title);
    });

    connect(m_view, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
        m_url = network::Url::parse(url.toString());
        emit urlChanged(m_url);
    });

    connect(m_view, &QWebEngineView::loadStarted, this, [this] {
        m_loading = true;
        emit loadStarted();
    });

    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_loading = false;
        recordVisit();
        emit loadFinished(ok);
    });

    connect(m_view, &QWebEngineView::loadProgress, this,
            [this](int progress) { emit loadProgress(progress); });

    connect(m_page, &QWebEnginePage::linkHovered, this,
            [this](const QString &url) { emit linkHovered(url); });

    if (auto *webView = qobject_cast<WebView *>(m_view)) {
        connect(webView, &WebView::newViewRequested, this,
                [this](QWebEngineView *view) { emit newViewRequested(view); });
    }
}

void WebTab::navigate(const network::Url &url)
{
    if (!url.isValid())
        return;

    if (url.isAbout()) {
        loadAbout(url);
        return;
    }

    m_url = url;
    m_view->load(QUrl(url.toString()));
}

void WebTab::navigateToUserInput(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return;

    // A URL is tried first so that a host name is never searched for.
    if (network::Url url = network::Url::fromUserInput(trimmed, m_url); url.isValid()) {
        navigate(url);
        return;
    }

    if (trimmed.startsWith(QLatin1String("about:"), Qt::CaseInsensitive)) {
        navigate(network::Url::parse(trimmed));
        return;
    }

    navigate(network::Url::forSearchQuery(trimmed, m_searchTemplate));
}

void WebTab::back()
{
    if (m_view)
        m_view->back();
}

void WebTab::forward()
{
    if (m_view)
        m_view->forward();
}

void WebTab::reload()
{
    if (m_view)
        m_view->reload();
}

void WebTab::stop()
{
    if (m_view)
        m_view->stop();
}

void WebTab::setHistory(storage::HistoryStore *history)
{
    m_history = history;
}

void WebTab::setBookmarks(storage::BookmarkStore *bookmarks)
{
    m_bookmarks = bookmarks;
}

void WebTab::setTabCount(int count)
{
    m_tabCount = count;
}

void WebTab::setSearchTemplate(const QString &searchTemplate)
{
    m_searchTemplate = searchTemplate;
}

void WebTab::attachDevTools(QWebEnginePage *devToolsPage)
{
    if (m_page && devToolsPage)
        m_page->setDevToolsPage(devToolsPage);
}

void WebTab::detachDevTools()
{
    if (m_page)
        m_page->setDevToolsPage(nullptr);
}

void WebTab::loadAbout(const network::Url &url)
{
    const QString html = browser::builtin::documentFor(
        url, QStringLiteral(OPENQBROWSER_VERSION), 1280, m_history, m_bookmarks, m_tabCount);

    m_url = url;
    m_view->setHtml(html, QUrl(QStringLiteral("about:") + url.aboutPage()));
}

void WebTab::recordVisit()
{
    if (m_history && m_url.isValid() && !m_url.isAbout())
        m_history->visit(m_url, m_title);
}

} // namespace oqb::ui

#include "WebTab.moc"
