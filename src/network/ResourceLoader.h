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
#include "network/ScriptFetch.h"
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
    /// The response headers as received. Script reads them through a Response's
    /// Headers object, and a page that cannot see `Content-Type` cannot decide
    /// how to parse what it was given.
    HeaderList headers;
    /// The status text, kept because a Response exposes it and the reason phrase
    /// is not recoverable from the code alone.
    QString statusText;
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
///
/// It is also the ScriptFetchProvider page script's fetch() talks to, so a
/// request a page makes from JavaScript goes through the same cache, the same
/// cookie jar, the same security policy and the same connection limit as a
/// request the page itself made.
class ResourceLoader : public ScriptFetchProvider
{
    Q_OBJECT

public:
    explicit ResourceLoader(QObject *parent = nullptr);
    ~ResourceLoader() override;

    /// Fetches `url`; finished() is emitted later. It may fire synchronously
    /// from inside this call when the resource is already cached.
    void fetch(const Url &url, const QString &referrer = {});

    /// A request made by page script rather than by the page itself.
    using ScriptRequest = network::ScriptFetchRequest;

    /// Fetches a resource on behalf of page script, returning an id that
    /// identifies this request. See ScriptFetchProvider for the contract.
    ///
    /// Script requests deliberately do not use finished(): a page's own loader
    /// handler routes every finished() response as a subresource it is waiting
    /// for, so a fetch() would decrement the page's in-flight counter, be treated
    /// as a stylesheet to apply or an image to measure, and on a page with no
    /// other work could end the load early. Script completion is reported on its
    /// own signal, keyed by the returned id, which no page code watches.
    int startScriptFetch(const ScriptRequest &request) override;

    /// Abandons a script request. It stays silent afterwards, which is what
    /// makes an aborted fetch not resolve.
    void abortScriptFetch(int requestId) override;

    /// Why the last startScriptFetch() call was refused. Empty when it was not.
    QString refusalReason() const override { return m_scriptRefusal; }

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

    // fetchFinished() and fetchRedirected() come from ScriptFetchProvider, which
    // this class implements. They are declared once, on the interface, and
    // emitted from here: redeclaring them would hide the base signals moc
    // generates for the same signatures.

private:
    struct PendingRequest
    {
        Url url;
        QString referrer;
    };

    void pump();
    /// Wires a client's completion back to the loader. Every request path goes
    /// through this, so no path can create a client that nothing listens to.
    void connectClient(HttpClient *client);
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

    /// How a request that has come back should be reported.
    ///
    /// Script requests are tracked in a map parallel to m_clients, so the page
    /// path is untouched: an ordinary request is still identified by its URL
    /// alone, and only a request script made carries the extra bookkeeping the
    /// CORS decision and the promise need.
    struct ScriptMeta
    {
        /// The id startScriptFetch() handed out. Never 0.
        int requestId = 0;
        /// The document's origin, which a CORS grant is compared against.
        QString requestOrigin;
        /// True when the request asked for cookies, so a wildcard CORS grant
        /// cannot authorise reading the response.
        bool withCredentials = false;
        /// True when the request asked for no cookies at all, in which case even
        /// the ones a redirect would re-add are suppressed.
        bool noCredentials = false;
        /// True when the document and the request share an origin, in which case
        /// no CORS check applies and every header is readable.
        bool sameOrigin = true;
        /// The body length, kept so the network view can show it without holding
        /// the body itself.
        int length = 0;
        Url url;
    };

    /// Starts a script request, which carries its own method, headers and body.
    void startScriptRequest(const ScriptMeta &meta, const ScriptRequest &request);
    /// Completes a script request: absorbs its cookies, applies the CORS
    /// decision, and reports it on fetchFinished().
    void handleScriptResponse(const ScriptMeta &meta, const HttpResponse &response);
    /// Reports a script request that produced no response at all.
    void failScriptRequest(const ScriptMeta &meta, const QString &error);

    /// Converts a stored or freshly parsed Resource into the shape script sees.
    static ScriptFetchResponse scriptResponseFromResource(const Resource &resource,
                                                          const ScriptMeta &meta);
    /// Converts a redirect hop, which is reported without a body because the hop
    /// is discarded and the response that follows carries the content.
    ScriptFetchResponse redirectResponse(const ScriptMeta &meta,
                                         const HttpResponse &response) const;

    QHash<QString, CacheEntry> m_cache;
    QList<PendingRequest> m_queue;
    QHash<HttpClient *, Url> m_clients;
    /// The script requests that are in flight. A request appears in both this map
    /// and m_clients, which is what lets the shared completion path tell the two
    /// kinds apart.
    QHash<HttpClient *, ScriptMeta> m_scriptClients;
    /// Script requests accepted but not yet started, waiting for a free slot.
    QList<QPair<ScriptMeta, ScriptRequest>> m_scriptQueue;
    /// The next id fetchForScript() hands out. Starts at 1, because 0 is what a
    /// refused request returns.
    int m_nextScriptRequestId = 1;
    QString m_scriptRefusal;
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
