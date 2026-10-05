#include "storage/Bookmarks.h"

#include <QDateTime>
#include <QSet>

namespace oqb::storage {
namespace {

/// Escapes text for inclusion in the generated about: page.
QString escape(const QString &text)
{
    QString out = text;
    out.replace(QLatin1String("&"), QLatin1String("&amp;"));
    out.replace(QLatin1String("<"), QLatin1String("&lt;"));
    out.replace(QLatin1String(">"), QLatin1String("&gt;"));
    out.replace(QLatin1String("\""), QLatin1String("&quot;"));
    return out;
}

} // namespace

void BookmarkStore::add(const network::Url &url, const QString &title, const QString &folder)
{
    if (!url.isValid())
        return;

    for (Bookmark &bookmark : m_bookmarks) {
        if (bookmark.url.toString() == url.toString()) {
            // Update rather than duplicate, so a re-save refreshes the title.
            if (!title.isEmpty())
                bookmark.title = title;
            if (!folder.isEmpty())
                bookmark.folder = folder;
            return;
        }
    }

    Bookmark bookmark;
    bookmark.url = url;
    bookmark.title = title.isEmpty() ? url.displayHost() : title;
    bookmark.addedAt = QDateTime::currentMSecsSinceEpoch();
    bookmark.folder = folder;
    m_bookmarks.append(bookmark);
}

bool BookmarkStore::remove(const network::Url &url)
{
    for (int i = 0; i < m_bookmarks.size(); ++i) {
        if (m_bookmarks.at(i).url.toString() == url.toString()) {
            m_bookmarks.removeAt(i);
            return true;
        }
    }
    return false;
}

bool BookmarkStore::contains(const network::Url &url) const
{
    for (const Bookmark &bookmark : m_bookmarks) {
        if (bookmark.url.toString() == url.toString())
            return true;
    }
    return false;
}

bool BookmarkStore::toggle(const network::Url &url, const QString &title)
{
    if (contains(url)) {
        remove(url);
        return false;
    }
    add(url, title);
    return true;
}

QList<Bookmark> BookmarkStore::inFolder(const QString &folder) const
{
    if (folder.isEmpty())
        return m_bookmarks;

    QList<Bookmark> out;
    for (const Bookmark &bookmark : m_bookmarks) {
        if (bookmark.folder == folder)
            out.append(bookmark);
    }
    return out;
}

QStringList BookmarkStore::folders() const
{
    QSet<QString> names;
    for (const Bookmark &bookmark : m_bookmarks) {
        if (!bookmark.folder.isEmpty())
            names.insert(bookmark.folder);
    }
    QStringList sorted = names.values();
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}

QList<Bookmark> BookmarkStore::search(const QString &text, int limit) const
{
    QList<Bookmark> out;
    const QString needle = text.trimmed();

    for (int i = static_cast<int>(m_bookmarks.size()) - 1; i >= 0 && out.size() < limit; --i) {
        const Bookmark &bookmark = m_bookmarks.at(i);
        if (needle.isEmpty() || bookmark.title.contains(needle, Qt::CaseInsensitive)
            || bookmark.url.toString().contains(needle, Qt::CaseInsensitive)) {
            out.append(bookmark);
        }
    }

    return out;
}

void BookmarkStore::clear()
{
    m_bookmarks.clear();
}

QString BookmarkStore::toHtml() const
{
    if (m_bookmarks.isEmpty()) {
        return QStringLiteral("<h1>Bookmarks</h1>"
                              "<p>No bookmarks yet. Use the star button in the toolbar to "
                              "save the page you are on.</p>");
    }

    QString html = QStringLiteral("<h1>Bookmarks</h1>\n");

    const QStringList folderNames = folders();
    if (!folderNames.isEmpty()) {
        for (const QString &folder : folderNames) {
            html += QStringLiteral("<h2>%1</h2>\n<ul>\n").arg(escape(folder));
            for (const Bookmark &bookmark : inFolder(folder)) {
                html += QStringLiteral("  <li><a href=\"%1\">%2</a></li>\n")
                            .arg(escape(bookmark.url.toString()), escape(bookmark.title));
            }
            html += QStringLiteral("</ul>\n");
        }
    }

    const QList<Bookmark> topLevel = inFolder({});
    if (!topLevel.isEmpty()) {
        if (!folderNames.isEmpty())
            html += QStringLiteral("<h2>Other</h2>\n");
        html += QStringLiteral("<ul>\n");
        for (const Bookmark &bookmark : topLevel) {
            html += QStringLiteral("  <li><a href=\"%1\">%2</a></li>\n")
                        .arg(escape(bookmark.url.toString()), escape(bookmark.title));
        }
        html += QStringLiteral("</ul>\n");
    }

    return html;
}

} // namespace oqb::storage
