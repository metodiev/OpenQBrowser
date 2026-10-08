#include "ui/WebTab.h"

#include "browser/BuiltinPages.h"
#include "storage/Bookmarks.h"
#include "storage/History.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>

namespace oqb::ui {

namespace {

/// The page icon cache, keyed by the icon URL.
///
/// Tab icons are fetched here rather than through QWebEngineView::iconChanged,
/// which does not report icons in this Qt build. The cache matters because the
/// same favicon is asked for by every tab on the site, and a news site's front
/// page can open several.
struct IconCache
{
    static constexpr int kMaxEntries = 128;
    QHash<QString, QIcon> icons;

    QIcon value(const QString &key) const { return icons.value(key); }
    void store(const QString &key, const QIcon &icon)
    {
        if (icon.isNull())
            return;
        // Wholesale clearing keeps eviction to one branch; a favicon is cheap
        // to fetch again compared with ranking entries.
        if (icons.size() >= kMaxEntries && !icons.contains(key))
            icons.clear();
        icons.insert(key, icon);
    }
};

IconCache &iconCache()
{
    static IconCache cache;
    return cache;
}

/// A QNetworkAccessManager for icons alone.
///
/// Qt WebEngine keeps its network stack to itself, so the request is made with
/// Qt Network instead. That is enough for an image whose URL the page itself
/// named, and it keeps the fetch off the renderer.
QNetworkAccessManager *iconNetwork()
{
    static QNetworkAccessManager manager;
    return &manager;
}

/// Decodes an image from a data: URL. QNetworkAccessManager refuses the data
/// scheme, and an inlined icon is common on pages that care about round trips.
QIcon iconFromDataUrl(const QString &url)
{
    const int comma = url.indexOf(u',');
    if (comma < 0)
        return {};

    const QString header = url.mid(5, comma - 5); // Past "data:"
    const QByteArray payload = url.mid(comma + 1).toLatin1();
    const QByteArray data = header.contains(QLatin1String(";base64"), Qt::CaseInsensitive)
        ? QByteArray::fromBase64(payload)
        : QByteArray::fromPercentEncoding(payload);

    QPixmap pixmap;
    if (!pixmap.loadFromData(data) || pixmap.isNull())
        return {};
    return QIcon(pixmap);
}

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
    // Without this Chromium reports no icons at all, so the tab strip never
    // learns what a page's favicon is.
    m_page->settings()->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, true);
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
        // A navigation starts a new icon question; the answer from the previous
        // page must not be applied to this one.
        ++m_iconGeneration;
        m_iconSource.clear();
        emit loadStarted();
    });

    connect(m_view, &QWebEngineView::loadFinished, this, [this](bool ok) {
        m_loading = false;
        recordVisit();
        if (ok)
            refreshIcon();
        emit loadFinished(ok);
    });

    connect(m_view, &QWebEngineView::loadProgress, this,
            [this](int progress) { emit loadProgress(progress); });

    connect(m_view, &QWebEngineView::iconChanged, this,
            [this](const QIcon &icon) { emit iconChanged(icon); });

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

void WebTab::refreshIcon()
{
    // The page is asked which icon it declares, because the answer is whatever
    // the document says rather than a name guessed from the origin. The
    // fallback is the conventional /favicon.ico, which a great many sites rely
    // on without writing a <link>.
    static const char *kScript = R"(
        (function () {
            var links = document.querySelectorAll(
                'link[rel~="icon"], link[rel="shortcut icon"], link[rel="apple-touch-icon"]');
            var best = null;
            for (var i = 0; i < links.length; ++i) {
                var href = links[i].href;
                if (!href)
                    continue;
                // An apple-touch-icon is a better fallback than nothing, but a
                // real icon is preferred.
                if (!best || links[i].rel.indexOf('apple') === -1)
                    best = href;
                if (links[i].rel.indexOf('apple') === -1)
                    break;
            }
            if (best)
                return best;
            return location.origin + '/favicon.ico';
        })();
    )";

    const quint64 generation = m_iconGeneration;
    m_page->runJavaScript(QString::fromUtf8(kScript), [this, generation](const QVariant &value) {
        // A tab can navigate while the script is in flight, and an icon from
        // the page that is being left is worse than none.
        if (generation != m_iconGeneration)
            return;

        const QString iconUrl = value.toString();
        if (iconUrl.isEmpty() || !iconUrl.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
            // A data: icon is decoded here because the network stack does not
            // speak the scheme.
            if (iconUrl.startsWith(QLatin1String("data:"), Qt::CaseInsensitive))
                applyIcon(iconUrl, iconFromDataUrl(iconUrl));
            return;
        }

        applyIcon(iconUrl, iconCache().value(iconUrl));
        if (!iconCache().value(iconUrl).isNull())
            return;

        QNetworkRequest request{QUrl(iconUrl)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenQBrowser/1.0"));

        QNetworkReply *reply = iconNetwork()->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, iconUrl, generation] {
            reply->deleteLater();
            if (generation != m_iconGeneration)
                return;
            if (reply->error() != QNetworkReply::NoError)
                return;

            QPixmap pixmap;
            if (!pixmap.loadFromData(reply->readAll()) || pixmap.isNull())
                return;

            const QIcon icon(pixmap);
            iconCache().store(iconUrl, icon);
            applyIcon(iconUrl, icon);
        });
    });
}

void WebTab::applyIcon(const QString &source, const QIcon &icon)
{
    if (icon.isNull() || icon.availableSizes().isEmpty())
        return;
    if (source == m_iconSource)
        return;

    m_iconSource = source;
    emit iconChanged(icon);
}

} // namespace oqb::ui

#include "WebTab.moc"
