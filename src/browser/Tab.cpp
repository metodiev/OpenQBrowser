#include "browser/Tab.h"

#include "storage/History.h"

namespace oqb::browser {

Tab::Tab(const PageSettings &settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_page(std::make_unique<Page>(settings, this))
{
    connectPage();
}

Tab::~Tab() = default;

void Tab::connectPage()
{
    connect(m_page.get(), &Page::started, this, [this] {
        emit loadStarted();
        emit stateChanged();
    });

    connect(m_page.get(), &Page::ready, this, [this] {
        // The title is only known once the document has been parsed.
        emit titleChanged(m_page->title());
        emit stateChanged();
    });

    connect(m_page.get(), &Page::finished, this, [this] {
        // The Page records the visit itself, because it is the thing that knows
        // whether the load ended on an error page.
        emit loadFinished();
        emit titleChanged(m_page->title());
        emit stateChanged();
    });

    connect(m_page.get(), &Page::failed, this, [this](const QString &message) {
        emit loadFailed(message);
        emit stateChanged();
    });

    connect(m_page.get(), &Page::titleChanged, this, [this](const QString &title) {
        emit titleChanged(title);
    });
}

void Tab::setSettings(const PageSettings &settings)
{
    m_settings = settings;
    m_page->setSettings(settings);
}

void Tab::setHistory(storage::HistoryStore *history)
{
    m_history = history;
    m_page->setHistory(history);
}

void Tab::setBookmarks(storage::BookmarkStore *bookmarks)
{
    m_page->setBookmarks(bookmarks);
}

void Tab::navigate(const network::Url &url)
{
    if (!url.isValid())
        return;
    m_navigatingFromHistory = false;
    m_page->load(url);
}

void Tab::reload()
{
    if (m_page->url().isValid())
        m_page->reload();
}

void Tab::stop()
{
    m_page->stop();
    emit stateChanged();
}

bool Tab::canGoBack() const
{
    return m_history && m_history->canGoBack();
}

bool Tab::canGoForward() const
{
    return m_history && m_history->canGoForward();
}

bool Tab::goBack()
{
    if (!canGoBack())
        return false;

    m_history->goBack();
    const network::Url url = m_history->currentUrl();
    if (!url.isValid())
        return false;

    // The cursor has already moved, so loading this URL must not add a step.
    m_navigatingFromHistory = true;
    m_page->load(url);
    return true;
}

bool Tab::goForward()
{
    if (!canGoForward())
        return false;

    m_history->goForward();
    const network::Url url = m_history->currentUrl();
    if (!url.isValid())
        return false;

    m_navigatingFromHistory = true;
    m_page->load(url);
    return true;
}

bool Tab::navigateToUserInput(const QString &input)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
        return false;

    // The address bar accepts three things: a URL, a known about: page, or a
    // search phrase. A URL is tried first so that a host name is never searched for.
    if (network::Url url = network::Url::fromUserInput(trimmed, m_page->url()); url.isValid()) {
        navigate(url);
        return true;
    }

    if (trimmed.startsWith(QLatin1String("about:"), Qt::CaseInsensitive)) {
        navigate(network::Url::parse(trimmed));
        return true;
    }

    navigate(network::Url::forSearchQuery(trimmed, m_settings.searchTemplate));
    return true;
}

} // namespace oqb::browser
