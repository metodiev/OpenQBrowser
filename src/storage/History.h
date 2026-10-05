#pragma once

#include <QList>
#include <QString>

#include "network/Url.h"

namespace oqb::storage {

/// One entry in the history.
///
/// The history stores a title and a timestamp as well as the URL, because those
/// are what the history panel and the autocomplete need; a bare URL list would
/// have to be re-fetched to be useful.
struct HistoryEntry
{
    network::Url url;
    QString title;
    qint64 visitedAt = 0; ///< Milliseconds since the epoch.
    int visitCount = 1;
};

/// The list of pages the user has visited, with back, forward and a search.
///
/// This is the model a tab's back and forward buttons drive: a tab holds an
/// index into this list rather than its own copy of it, which is what keeps the
/// two in agreement.
class HistoryStore
{
public:
    /// Maximum entries kept per session, so a long run cannot grow without bound.
    static constexpr int kMaxEntries = 5000;

    /// Records a visit. Visiting the current entry again only updates its title
    /// and count; navigating to a new URL truncates the forward entries, which
    /// is what every browser does.
    void visit(const network::Url &url, const QString &title = {});
    /// Replaces the title of the current entry, used when a page loads late.
    void updateCurrentTitle(const QString &title);

    int count() const { return static_cast<int>(m_entries.size()); }
    HistoryEntry at(int index) const;
    const QList<HistoryEntry> &entries() const { return m_entries; }

    int currentIndex() const { return m_currentIndex; }
    bool canGoBack() const { return m_currentIndex > 0; }
    bool canGoForward() const { return m_currentIndex + 1 < count(); }

    /// Moves the cursor without recording a new visit. Returns false when the
    /// move is not possible.
    bool goBack();
    bool goForward();
    /// Moves the cursor to `index`; returns false when it is out of range.
    bool goTo(int index);

    network::Url currentUrl() const;
    QString currentTitle() const;

    /// Empties the history, as a private browsing window does when it closes.
    void clear();

    /// Entries whose URL or title contain `text`, most recent first, at most
    /// `limit` of them. This is what the address bar dropdown shows.
    QList<HistoryEntry> search(const QString &text, int limit = 10) const;

    /// The most recently visited distinct URLs, most recent first.
    QList<network::Url> mostVisited(int limit = 10) const;

    /// All history, oldest first, as a plain text listing for about:history.
    QString describe() const;

private:
    QList<HistoryEntry> m_entries;
    int m_currentIndex = -1;
};

} // namespace oqb::storage
