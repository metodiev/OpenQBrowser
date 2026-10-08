#pragma once

#include <QIcon>
#include <QWidget>

#include "network/Url.h"

class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

namespace oqb::storage {
class BookmarkStore;
class HistoryStore;
}

namespace oqb::ui {

/// One browser tab, rendered by Qt WebEngine (Chromium).
///
/// A WebTab is the widget placed in the tab bar; it contains a QWebEngineView
/// and translates between it and the rest of the window: it resolves what the
/// user typed into an address, serves the built-in about: pages, records visits
/// and forwards the view's signals in the shapes the window understands.
class WebTab : public QWidget
{
    Q_OBJECT

public:
    /// Creates a tab with its own view and page on `profile`.
    explicit WebTab(QWebEngineProfile *profile, QWidget *parent = nullptr);
    /// Wraps a view created elsewhere (a target=_blank or window.open), sharing
    /// the profile so cookies and cache are common to the window.
    WebTab(QWebEngineView *view, QWebEngineProfile *profile, QWidget *parent = nullptr);
    ~WebTab() override;

    QWebEngineView *view() const { return m_view; }
    QWebEnginePage *page() const { return m_page; }

    /// Loads `url`: a network page, or a generated document for about: URLs.
    void navigate(const network::Url &url);
    /// Turns address-bar text into a URL or a search and navigates.
    void navigateToUserInput(const QString &input);

    network::Url url() const { return m_url; }
    QString title() const { return m_title; }
    bool isLoading() const { return m_loading; }

    void back();
    void forward();
    void reload();
    void stop();

    /// Stores used for the built-in pages and for recording visits.
    void setHistory(storage::HistoryStore *history);
    void setBookmarks(storage::BookmarkStore *bookmarks);
    void setTabCount(int count);
    void setSearchTemplate(const QString &searchTemplate);

    /// Points the window's inspector at this tab, or detaches it.
    void attachDevTools(QWebEnginePage *devToolsPage);
    void detachDevTools();

signals:
    void titleChanged(const QString &title);
    void urlChanged(const network::Url &url);
    void loadStarted();
    void loadFinished(bool ok);
    void loadProgress(int progress);
    void linkHovered(const QString &url);
    /// The page's own icon, for the tab strip. A page without one reports an
    /// empty icon, which the tab then draws without.
    void iconChanged(const QIcon &icon);
    /// The view a popup or target=_blank asked for; the window adopts it as a
    /// new tab.
    void newViewRequested(QWebEngineView *view);

private:
    /// Wires the view's signals once, for both constructors.
    void connectView();
    /// Loads the generated document for an about: URL.
    void loadAbout(const network::Url &url);
    /// Records the finished load in the history store.
    void recordVisit();
    /// Asks the page which icon it declares and fetches it for the tab strip.
    void refreshIcon();
    /// Reports an icon for `source` if it is new, so the tab strip is not
    /// repainted for an icon it already shows.
    void applyIcon(const QString &source, const QIcon &icon);

    QWebEngineView *m_view = nullptr;
    QWebEnginePage *m_page = nullptr;
    QWebEngineProfile *m_profile = nullptr;

    storage::HistoryStore *m_history = nullptr;
    storage::BookmarkStore *m_bookmarks = nullptr;
    int m_tabCount = 1;
    QString m_searchTemplate;

    network::Url m_url;
    QString m_title;
    bool m_loading = false;
    /// Incremented by every navigation, so an icon fetched for the previous
    /// page is discarded rather than applied to the new one.
    quint64 m_iconGeneration = 0;
    /// The icon URL currently in use, so the same icon is not re-emitted.
    QString m_iconSource;
};

} // namespace oqb::ui
