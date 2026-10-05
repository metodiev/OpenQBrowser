#include "storage/History.h"

#include <QDateTime>
#include <QHash>

#include <algorithm>

namespace oqb::storage {

void HistoryStore::visit(const network::Url &url, const QString &title)
{
    if (!url.isValid())
        return;

    const QString key = url.toString();

    // Re-visiting the current entry is a reload, not a new step: it updates the
    // existing entry rather than adding a duplicate.
    if (m_currentIndex >= 0 && m_currentIndex < count()
        && m_entries.at(m_currentIndex).url.toString() == key) {
        HistoryEntry &entry = m_entries[m_currentIndex];
        entry.visitedAt = QDateTime::currentMSecsSinceEpoch();
        ++entry.visitCount;
        if (!title.isEmpty())
            entry.title = title;
        return;
    }

    // Navigating after going back discards the forward entries, exactly as a
    // browser's history does.
    while (m_entries.size() > m_currentIndex + 1)
        m_entries.removeLast();

    HistoryEntry entry;
    entry.url = url;
    entry.title = title;
    entry.visitedAt = QDateTime::currentMSecsSinceEpoch();
    m_entries.append(entry);
    m_currentIndex = count() - 1;

    while (m_entries.size() > kMaxEntries) {
        m_entries.removeFirst();
        --m_currentIndex;
    }
}

void HistoryStore::updateCurrentTitle(const QString &title)
{
    if (m_currentIndex < 0 || m_currentIndex >= count() || title.isEmpty())
        return;
    m_entries[m_currentIndex].title = title;
}

HistoryEntry HistoryStore::at(int index) const
{
    if (index < 0 || index >= count())
        return {};
    return m_entries.at(index);
}

bool HistoryStore::goBack()
{
    if (!canGoBack())
        return false;
    --m_currentIndex;
    return true;
}

bool HistoryStore::goForward()
{
    if (!canGoForward())
        return false;
    ++m_currentIndex;
    return true;
}

bool HistoryStore::goTo(int index)
{
    if (index < 0 || index >= count())
        return false;
    m_currentIndex = index;
    return true;
}

network::Url HistoryStore::currentUrl() const
{
    return m_currentIndex >= 0 && m_currentIndex < count() ? m_entries.at(m_currentIndex).url
                                                           : network::Url();
}

QString HistoryStore::currentTitle() const
{
    return m_currentIndex >= 0 && m_currentIndex < count() ? m_entries.at(m_currentIndex).title
                                                           : QString();
}

void HistoryStore::clear()
{
    m_entries.clear();
    m_currentIndex = -1;
}

QList<HistoryEntry> HistoryStore::search(const QString &text, int limit) const
{
    QList<HistoryEntry> results;
    const QString needle = text.trimmed();

    if (needle.isEmpty()) {
        // With nothing to match on, the most recent entries are the useful ones.
        QList<HistoryEntry> recent;
        for (int i = count() - 1; i >= 0 && recent.size() < limit; --i)
            recent.append(m_entries.at(i));
        return recent;
    }

    // Most recent first: a user looking for a page usually wants the last visit.
    for (int i = count() - 1; i >= 0 && results.size() < limit; --i) {
        const HistoryEntry &entry = m_entries.at(i);
        if (entry.url.toString().contains(needle, Qt::CaseInsensitive)
            || entry.title.contains(needle, Qt::CaseInsensitive)) {
            results.append(entry);
        }
    }

    return results;
}

QList<network::Url> HistoryStore::mostVisited(int limit) const
{
    // Count visits per URL, then take the largest counts, breaking ties by the
    // more recent visit so that a freshly used page ranks above an old one.
    struct Tally
    {
        network::Url url;
        int visits = 0;
        qint64 lastVisit = 0;
    };

    QHash<QString, Tally> tallies;
    for (const HistoryEntry &entry : m_entries) {
        const QString key = entry.url.toString();
        Tally &tally = tallies[key];
        tally.url = entry.url;
        tally.visits += entry.visitCount;
        tally.lastVisit = qMax(tally.lastVisit, entry.visitedAt);
    }

    QList<Tally> ordered = tallies.values().toVector().toList();
    std::sort(ordered.begin(), ordered.end(), [](const Tally &a, const Tally &b) {
        if (a.visits != b.visits)
            return a.visits > b.visits;
        return a.lastVisit > b.lastVisit;
    });

    QList<network::Url> out;
    for (int i = 0; i < ordered.size() && i < limit; ++i)
        out.append(ordered.at(i).url);
    return out;
}

QString HistoryStore::describe() const
{
    QString out;
    for (int i = 0; i < count(); ++i) {
        const HistoryEntry &entry = m_entries.at(i);
        out += i == m_currentIndex ? QStringLiteral("* ") : QStringLiteral("  ");
        out += entry.title.isEmpty() ? entry.url.toString() : entry.title;
        out += QStringLiteral("  (") + entry.url.toString() + u')';
        if (entry.visitCount > 1)
            out += QStringLiteral(" [") + QString::number(entry.visitCount) + QStringLiteral(" visits]");
        out += u'\n';
    }
    return out;
}

} // namespace oqb::storage
