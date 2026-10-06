#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include "network/Cache.h"
#include "network/HttpMessage.h"
#include "network/Url.h"

namespace oqb::storage {
class CookieJar;
}

namespace oqb::network {

class HttpClient;

/// A resource fetched (or read from disk) on behalf of a page.
struct Resource
{
    Url url;
    QByteArray data;
    /// The URL the resource was requested at. A redirect answers at a different
    /// URL, so `url` alone cannot tell a caller whether this is the response to
    /// the request it made.
    Url requestedUrl;
    QString mimeType;
    QString error;
    int statusCode = 0;

    bool ok() const { return error.isEmpty(); }
    bool isHtml() const { return mimeType.startsWith(QLatin1String("text/html")); }
    bool isCss() const { return mimeType.startsWith(QLatin1String("text/css")); }
    bool isImage() const { return mimeType.startsWith(QLatin1String("image/")); }
    bool isScript() const
    {
        return mimeType.startsWith(QLatin1String("text/javascript"))
            || mimeType.startsWith(QLatin1String("application/javascript"));
    }

    /// The payload decoded to text, honouring a charset in the MIME type.
    QString text() const;
};

/// One stored response plus what is needed to decide whether it may be reused.
///
/// The `Resource` is kept whole because a revalidation answering 304 produces no
/// body: the stored one is what gets served, so discarding it would turn every
/// successful revalidation into an empty response.
struct CacheEntry
{
    Resource resource;
    /// When this browser stored the response. The freshness clock runs forward
    /// from here; `date` says when the origin generated it.
    QDateTime storedAt;
    /// The origin's `Date`, used to measure age correctly.
    QDateTime date;
    QDateTime lastModified;
    QDateTime expires;
    /// The `ETag` verbatim, including any `W/` prefix. It is an opaque token, and
    /// deciding whether two are equivalent is the server's job, not this cache's.
    QString etag;
    /// How long the response stays fresh, in seconds. 0 means "revalidate".
    int freshnessLifetime = 0;
    /// Stored, but must be confirmed with the server before every reuse.
    bool noCache = false;
    /// Must not be reused once stale without revalidation.
    bool mustRevalidate = false;
    /// Which document load stored this. A duplicate request within the same load
    /// is answered from the entry whatever its headers say, which is what stops
    /// one page referencing the same image ten times from making ten requests.
    /// Across loads the freshness rules apply instead.
    int generation = 0;

    /// True when the entry can be used without a request.
    bool isFresh(const QDateTime &now) const
    {
        return CachePolicy::isFresh(CachePolicy::ageSeconds(storedAt, date, now),
                                    freshnessLifetime, noCache);
    }

    /// True when the entry can be revalidated at all: with neither an ETag nor a
    /// Last-Modified there is nothing to send, so the origin must be asked for
    /// the whole response.
    bool canRevalidate() const { return !etag.isEmpty() || lastModified.isValid(); }
};

/// Fetches navigations and subresources on behalf of a page.
///
/// The loader owns an in-memory cache so a stylesheet or image referenced twice
/// is downloaded once, caps how many requests run in parallel, and emits
/// finished() exactly once per fetch() call. Failures are cached as well, so a
/// broken URL is not retried on every repaint.
class ResourceLoader : public QObject
{
    Q_OBJECT

public:
    explicit ResourceLoader(QObject *parent = nullptr);
    ~ResourceLoader() override;

    /// Fetches `url`; finished() is emitted later. It may fire synchronously
    /// from inside this call when the resource is already cached.
    void fetch(const Url &url, const QString &referrer = {});

    /// Reads a file:// URL from disk. Used by fetch() and by unit tests.
    static Resource loadLocalFile(const Url &url);

    bool cached(const Url &url, Resource *out) const;
    void store(const Resource &resource);
    void clearCache();

    int cacheSize() const { return m_cache.size(); }

    /// How many responses were served from cache without a request, and how many
    /// were confirmed with a 304. Reported by the DevTools network view.
    int cacheHitCount() const { return m_cacheHits; }
    int revalidationCount() const { return m_revalidations; }

    /// Empties the cache and forgets the counters.
    void resetCacheCounters();

    /// True when `url` has a stored entry that may be served as it stands.
    bool isFreshInCache(const Url &url) const;

    /// Marks the start of a new document load.
    ///
    /// Entries from the previous load stop being reused for request collapsing
    /// but remain available under the HTTP freshness rules. This is what lets a
    /// stylesheet referenced by two pages be revalidated rather than downloaded
    /// again, while a broken image on one page is still not retried for every
    /// element that points at it.
    void beginLoad();

    /// Marks the next fetch of `url` as a reload: any stored entry is revalidated
    /// rather than reused, as the reload button requires.
    void invalidate(const Url &url);
    void setMaxConcurrentRequests(int count) { m_maxConcurrent = qMax(1, count); }
    int requestCount() const { return m_requestCount; }

    void setUserAgent(const QString &userAgent) { m_userAgent = userAgent; }

    /// The cookie jar requests read from and responses write to.
    ///
    /// The jar is owned by the browser rather than by the loader, because it has
    /// to outlive a single page: that is what makes a login survive a
    /// navigation. A null jar means no cookies are sent or stored, which is what
    /// a loader built on its own gets.
    void setCookieJar(storage::CookieJar *jar) { m_cookies = jar; }
    storage::CookieJar *cookieJar() const { return m_cookies; }

    /// Milliseconds before a request is abandoned; 0 disables the timeout.
    void setRequestTimeout(int milliseconds) { m_requestTimeoutMs = milliseconds; }

    /// Aborts every in-flight request without emitting further signals.
    void cancelAll();

signals:
    void finished(const oqb::network::Resource &resource);

private:
    struct PendingRequest
    {
        Url url;
        QString referrer;
    };

    void pump();
    void startRequest(const PendingRequest &request);
    void handleHttpResponse(const Url &url, const HttpResponse &response);
    void handleFailure(const Url &url, const QString &error);
    /// Merges a 304 into the entry it validates and answers with the stored body.
    void handleNotModified(const Url &url, const HttpResponse &response);

    /// Adds the `If-None-Match` / `If-Modified-Since` headers a stored entry
    /// needs, and returns whether either was added.
    bool applyValidators(HttpRequest *request, const Url &url) const;

    /// True when a stored entry may be reused without asking the server.
    bool reusable(const CacheEntry &entry, const QDateTime &now) const;

    /// Records `response` in the cache when its own headers permit it. Returns
    /// true when it was stored.
    bool absorbIntoCache(const Url &url, const HttpResponse &response, const Resource &resource);

    /// Tries to answer `request` from the cache. Returns true when it was
    /// answered, either from a fresh entry or after a revalidation round trip.
    bool tryCache(const PendingRequest &request);

    /// Adds the `Cookie` header for `request` from the jar, and refuses to add
    /// one the security policy would block.
    void applyCookies(HttpRequest *request, const Url &referrer) const;
    /// Stores the cookies a response set.
    void absorbCookies(const HttpResponse &response, const Url &url);

    QHash<QString, CacheEntry> m_cache;
    QList<PendingRequest> m_queue;
    QHash<HttpClient *, Url> m_clients;
    int m_maxConcurrent = 6;
    int m_requestTimeoutMs = 30000;
    int m_requestCount = 0;
    int m_cacheHits = 0;
    int m_revalidations = 0;
    /// Bumped by beginLoad(). Entries carry the generation that stored them.
    int m_generation = 0;
    /// URLs the next request for which must not be served from cache, set by
    /// invalidate() for a reload.
    QSet<QString> m_reloadUrls;
    /// Not owned. Null means cookies are disabled for this loader.
    storage::CookieJar *m_cookies = nullptr;
    QString m_userAgent;
};

} // namespace oqb::network
