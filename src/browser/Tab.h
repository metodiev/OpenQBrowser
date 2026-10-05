#pragma once

#include <QObject>
#include <QString>

#include <memory>

#include "browser/Page.h"

namespace oqb::storage {
class HistoryStore;
}

namespace oqb::browser {

/// One browser tab: a page plus the navigation state that belongs to it.
///
/// Back and forward move through the shared history store rather than keeping a
/// private list, so the tab's buttons and the history page always agree about
/// what the user has done.
class Tab : public QObject
{
    Q_OBJECT

public:
    explicit Tab(const PageSettings &settings = {}, QObject *parent = nullptr);
    ~Tab() override;

    /// Navigates to `url` and records it in the history.
    void navigate(const network::Url &url);
    /// Reloads the current page.
    void reload();
    /// Stops the load in progress.
    void stop();

    bool canGoBack() const;
    bool canGoForward() const;
    bool goBack();
    bool goForward();

    /// Navigates to a URL the user typed: a search phrase becomes a search,
    /// anything else becomes a navigation. Returns false when the input could
    /// not be interpreted at all.
    bool navigateToUserInput(const QString &input);

    Page *page() const { return m_page.get(); }
    const PageSettings &settings() const { return m_settings; }
    void setSettings(const PageSettings &settings);

    const network::Url &url() const { return m_page->url(); }
    QString title() const { return m_page->title(); }
    bool isLoading() const { return m_page->isLoading(); }

    void setHistory(storage::HistoryStore *history);
    void setBookmarks(storage::BookmarkStore *bookmarks);
    void setTabCount(int count) { m_page->setTabCount(count); }

signals:
    /// The page shown by this tab changed in a way the window must reflect:
    /// a new document, a redirect, a title or a load state change.
    void stateChanged();
    void titleChanged(const QString &title);
    void loadStarted();
    void loadFinished();
    void loadFailed(const QString &message);
    /// A link navigation was requested from inside the page.
    void navigationRequested(const oqb::network::Url &url, bool newTab);

private:
    void connectPage();

    PageSettings m_settings;
    std::unique_ptr<Page> m_page;
    storage::HistoryStore *m_history = nullptr;
    /// True while a history move is in progress, so the movement itself does not
    /// record another visit.
    bool m_navigatingFromHistory = false;
};

} // namespace oqb::browser
