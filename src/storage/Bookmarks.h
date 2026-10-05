#pragma once

#include <QList>
#include <QString>

#include "network/Url.h"

namespace oqb::storage {

/// A saved page.
struct Bookmark
{
    network::Url url;
    QString title;
    qint64 addedAt = 0;
    /// A folder name, or empty for the top level. Folders are created by using
    /// a name, which keeps the model small while still being organised.
    QString folder;
};

/// The user's saved pages.
///
/// Bookmarks live only in memory for now; persistence is planned alongside the
/// storage directory described in architecture/storage.md. Keeping the API
/// separate from the file format means adding persistence later does not disturb
/// the menus that use it.
class BookmarkStore
{
public:
    /// Saves `url`. Adding the same URL twice updates the existing entry rather
    /// than creating a duplicate.
    void add(const network::Url &url, const QString &title = {},
             const QString &folder = {});
    bool remove(const network::Url &url);
    bool contains(const network::Url &url) const;
    /// Toggles a bookmark and reports the new state.
    bool toggle(const network::Url &url, const QString &title = {});

    const QList<Bookmark> &bookmarks() const { return m_bookmarks; }
    int count() const { return static_cast<int>(m_bookmarks.size()); }
    bool isEmpty() const { return m_bookmarks.isEmpty(); }

    /// Bookmarks in `folder`, or every bookmark when `folder` is empty.
    QList<Bookmark> inFolder(const QString &folder) const;
    /// The distinct folder names, sorted.
    QStringList folders() const;

    /// Bookmarks whose title or URL matches `text`, most recently added first.
    QList<Bookmark> search(const QString &text, int limit = 10) const;

    void clear();

    /// The bookmarks as an HTML document, for about:bookmarks.
    QString toHtml() const;

private:
    QList<Bookmark> m_bookmarks;
};

} // namespace oqb::storage
