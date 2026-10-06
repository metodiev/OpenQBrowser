#include "network/ResourceLoader.h"

#include "security/SecurityPolicy.h"
#include "storage/Cookies.h"

#include "network/HttpClient.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStringDecoder>

namespace oqb::network {

/// How long a response confirmed by a 304 is treated as fresh when the 304
/// carried no lifetime of its own.
///
/// Without this, an entry the server has just confirmed would revalidate again
/// on the very next request: the 304 proved the body was current, so asking
/// immediately is a round trip that buys nothing. The value is deliberately
/// short, because the origin gave no lifetime and this is an inference, not an
/// instruction.
static constexpr int kConfirmedFreshnessSeconds = 60;

QString Resource::text() const
{
    // Subresources rarely declare a charset, and OpenQBrowser follows the web
    // convention of treating text from the network as UTF-8.
    const QString charset = http::splitContentType(mimeType).second;
    const QString codec = http::codecNameForCharset(charset);
    if (codec.compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) != 0
        && !charset.isEmpty()) {
        auto decoder = QStringDecoder(codec.toLatin1().constData());
        if (decoder.isValid())
            return decoder.decode(data);
    }
    return QString::fromUtf8(data);
}

ResourceLoader::ResourceLoader(QObject *parent)
    : QObject(parent)
{
}

ResourceLoader::~ResourceLoader() = default;

bool ResourceLoader::cached(const Url &url, Resource *out) const
{
    const auto it = m_cache.constFind(url.toString());
    if (it == m_cache.constEnd())
        return false;
    if (out)
        *out = it.value().resource;
    return true;
}

bool ResourceLoader::isFreshInCache(const Url &url) const
{
    const auto it = m_cache.constFind(url.toString());
    if (it == m_cache.constEnd())
        return false;
    // A URL marked for reload is not fresh however long its lifetime: that is
    // what makes the reload button reload.
    if (m_reloadUrls.contains(url.toString()))
        return false;
    return it.value().isFresh(QDateTime::currentDateTimeUtc());
}

void ResourceLoader::beginLoad()
{
    ++m_generation;
}

/// True when the entry may be reused without asking the server.
///
/// Within one load an entry is reused whatever its headers say, because a page
/// that references the same stylesheet ten times must make one request, and a
/// failed subresource must not be retried for every element pointing at it. The
/// entry is a memo of *this* load.
///
/// Across loads the headers decide, which is what makes a second visit to a page
/// cheap without serving something the origin said had expired.
bool ResourceLoader::reusable(const CacheEntry &entry, const QDateTime &now) const
{
    if (entry.generation == m_generation)
        return true;
    return entry.isFresh(now);
}

void ResourceLoader::invalidate(const Url &url)
{
    m_reloadUrls.insert(url.toString());
}

void ResourceLoader::clearCache()
{
    m_cache.clear();
    m_reloadUrls.clear();
}

void ResourceLoader::resetCacheCounters()
{
    m_cacheHits = 0;
    m_revalidations = 0;
}

void ResourceLoader::store(const Resource &resource)
{
    // A caller that supplies a Resource directly - a built-in page, a local file
    // - has no headers to obey, so the entry is stored as one that must be
    // revalidated. It carries no validator, so canRevalidate() is false and the
    // entry is simply never stale-served: it is written for callers of cached().
    CacheEntry entry;
    entry.resource = resource;
    entry.storedAt = QDateTime::currentDateTimeUtc();
    entry.freshnessLifetime = 0;
    entry.generation = m_generation;
    m_cache.insert(resource.url.toString(), entry);
}

bool ResourceLoader::absorbIntoCache(const Url &url, const HttpResponse &response,
                                     const Resource &resource)
{
    const CachePolicy::Directives directives = CachePolicy::parse(response.headers);
    if (!CachePolicy::isStorable(directives, response.statusCode))
        return false;

    // A response that varies by a header this cache does not key on must not be
    // stored, or a later request could be handed the wrong variant.
    if (!CachePolicy::varyIsHarmless(response.header(QStringLiteral("Vary"))))
        return false;

    CacheEntry entry;
    entry.resource = resource;
    entry.storedAt = QDateTime::currentDateTimeUtc();
    entry.date = CachePolicy::parseHttpDate(response.header(QStringLiteral("Date")));
    entry.lastModified = CachePolicy::parseHttpDate(response.header(QStringLiteral("Last-Modified")));
    entry.expires = CachePolicy::parseHttpDate(response.header(QStringLiteral("Expires")));
    entry.etag = response.header(QStringLiteral("ETag"));
    entry.noCache = directives.noCache;
    entry.mustRevalidate = directives.mustRevalidate;
    entry.freshnessLifetime = CachePolicy::freshnessLifetime(directives, entry.date,
                                                            entry.expires, entry.lastModified);
    entry.generation = m_generation;

    // A reload invalidated this URL, and the request that has just come back is
    // the answer to it, so the marker has served its purpose.
    m_reloadUrls.remove(url.toString());

    m_cache.insert(resource.url.toString(), entry);

    // A permanent redirect makes the target authoritative for the old URL too,
    // so later lookups by the original URL resolve without another round trip.
    if ((response.statusCode == 301 || response.statusCode == 308) && resource.url != url) {
        CacheEntry alias = entry;
        alias.resource.url = url;
        m_cache.insert(url.toString(), alias);
    }

    return true;
}

bool ResourceLoader::applyValidators(HttpRequest *request, const Url &url) const
{
    const auto it = m_cache.constFind(url.toString());
    if (it == m_cache.constEnd())
        return false;

    const CacheEntry &entry = it.value();
    bool added = false;

    // Both validators are sent when both are known. RFC 9110 makes a server that
    // receives both prefer the entity tag, so sending both is not ambiguous, and
    // it means an entry stored before either header was understood can still be
    // validated by whichever one it has.
    if (!entry.etag.isEmpty()) {
        request->headers.set(QStringLiteral("If-None-Match"), entry.etag);
        added = true;
    }
    if (entry.lastModified.isValid()) {
        request->headers.set(QStringLiteral("If-Modified-Since"),
                             CachePolicy::formatHttpDate(entry.lastModified));
        added = true;
    }

    return added;
}

bool ResourceLoader::tryCache(const PendingRequest &request)
{
    const QString key = request.url.toString();
    const auto it = m_cache.constFind(key);
    if (it == m_cache.constEnd())
        return false;

    // A reload must reach the origin, so the entry is never served as it stands.
    // The marker is deliberately *not* consumed here: startRequest() consults the
    // cache a second time after the queue, and clearing it now would let that
    // second look serve the very entry the reload was meant to bypass. It is
    // cleared when the fresh response arrives, or when the request fails.
    if (m_reloadUrls.contains(key)) {
        // Without a validator the entry cannot become a conditional request, so
        // it is dropped rather than left to be re-examined on every fetch.
        if (!it.value().canRevalidate())
            m_cache.remove(key);
        return false;
    }

    if (reusable(it.value(), QDateTime::currentDateTimeUtc())) {
        ++m_cacheHits;
        emit finished(it.value().resource);
        return true;
    }

    // Stale, and with no validator there is nothing to send with the request, so
    // the entry cannot be turned into a 304 and is dropped rather than left to
    // be re-examined on every later fetch.
    if (!it.value().canRevalidate())
        m_cache.remove(key);

    return false;
}

void ResourceLoader::cancelAll()
{
    const auto clients = m_clients;
    m_clients.clear();
    m_queue.clear();
    for (HttpClient *client : clients.keys())
        client->abort();
}

void ResourceLoader::fetch(const Url &url, const QString &referrer)
{
    if (!url.isValid()) {
        Resource resource;
        resource.url = url;
        resource.error = tr("Invalid URL");
        store(resource);
        emit finished(resource);
        return;
    }

    if (tryCache({url, referrer}))
        return;

    if (url.isLocalFile()) {
        const Resource resource = loadLocalFile(url);
        store(resource);
        emit finished(resource);
        return;
    }

    if (!url.isRemote()) {
        Resource resource;
        resource.url = url;
        resource.error = tr("OpenQBrowser cannot load %1 resources").arg(url.scheme());
        store(resource);
        emit finished(resource);
        return;
    }

    m_queue.append({url, referrer});
    pump();
}

void ResourceLoader::pump()
{
    while (!m_queue.isEmpty() && m_clients.size() < m_maxConcurrent)
        startRequest(m_queue.takeFirst());
}

void ResourceLoader::applyCookies(HttpRequest *request, const Url &referrer) const
{
    if (!m_cookies || !request)
        return;

    // The policy decides whether sending a cookie is acceptable, which is where
    // the SameSite rule and the secure-origin rule are enforced. A refused
    // request sends no cookie at all rather than a partial set, because a server
    // that receives half a session is worse off than one that receives none.
    if (referrer.isValid()) {
        const security::SecurityPolicy policy;
        if (!policy.canSendCookies(referrer, request->url).allowed)
            return;
    }

    const QString header = m_cookies->requestHeader(request->url);
    if (header.isEmpty())
        return;

    // set() rather than append(): a caller may have supplied a Cookie of its own,
    // and two Cookie headers are not equivalent to one.
    request->headers.set(QStringLiteral("Cookie"), header);
}

void ResourceLoader::absorbCookies(const HttpResponse &response, const Url &url)
{
    if (!m_cookies)
        return;

    // The cookies are stored against the URL the response came from, not the URL
    // that was requested: after a redirect the server that sent Set-Cookie is the
    // one at the final address, and those are the host and path the cookie
    // belongs to.
    const Url &source = response.finalUrl.isValid() ? response.finalUrl : url;
    m_cookies->storeFromHeaders(response.headers, source);
}

void ResourceLoader::startRequest(const PendingRequest &request)
{
    // Another request for the same URL may have completed while this one waited
    // in the queue, so the cache is consulted again here rather than only in
    // fetch(): a page that references one stylesheet ten times makes one request.
    if (tryCache(request))
        return;

    ++m_requestCount;

    auto *client = new HttpClient(this);
    m_clients.insert(client, request.url);

    connect(client, &HttpClient::finished, this, [this, client](const HttpResponse &response) {
        const Url url = m_clients.value(client);
        m_clients.remove(client);
        client->deleteLater();
        handleHttpResponse(url, response);
        pump();
    });

    connect(client, &HttpClient::failed, this, [this, client](const QString &error) {
        const Url url = m_clients.value(client);
        m_clients.remove(client);
        client->deleteLater();
        handleFailure(url, error);
        pump();
    });

    // A redirect response is followed inside HttpClient, so it never reaches
    // handleHttpResponse(). Its Set-Cookie headers still matter: a login that
    // redirects to the dashboard sets the session on the hop that is discarded.
    connect(client, &HttpClient::redirectResponse, this,
            [this, client](const HttpResponse &response) {
                absorbCookies(response, m_clients.value(client));
            });

    // Each hop asks the jar afresh which cookies belong on the request, because
    // the hop that just completed may have changed the answer. The referrer is
    // the original one: a redirect does not change what the navigation is, so
    // the SameSite decision must be made against the same context throughout.
    const Url referrer = request.referrer.isEmpty() ? Url() : Url::parse(request.referrer);
    client->setCookieProvider([this, referrer](const Url &url) {
        HttpRequest probe;
        probe.url = url;
        applyCookies(&probe, referrer);
        return probe.headers.joined(QStringLiteral("Cookie"));
    });

    HttpRequest httpRequest;
    httpRequest.url = request.url;
    httpRequest.userAgent = m_userAgent;
    // The referrer is the page that asked for the resource, and it is what the
    // SameSite decision is made against. It arrives as text, so it is parsed;
    // an unparseable or absent referrer is treated as no referrer.
    applyCookies(&httpRequest, referrer);
    // A stored entry turns this into a conditional request: the validators go out
    // and the server answers 304 when the body it holds is still current. This is
    // the whole point of the feature - a stale cache should cost one round trip
    // and no body, not a re-download.
    applyValidators(&httpRequest, request.url);
    // Without this the timeout on PageSettings would be a setting that does
    // nothing, and a server that never answers would hang the page forever.
    httpRequest.timeoutMs = m_requestTimeoutMs;
    if (!request.referrer.isEmpty())
        httpRequest.headers.append(QStringLiteral("Referer"), request.referrer);

    // Deferred so fetch() callers see the request leave asynchronously and the
    // event loop stays responsive while several requests fan out.
    QMetaObject::invokeMethod(client, [client, httpRequest] { client->send(httpRequest); },
                              Qt::QueuedConnection);
}

void ResourceLoader::handleHttpResponse(const Url &url, const HttpResponse &response)
{
    // Cookies are stored before anything else, because a response that is about
    // to be discarded as an error still sets them - which is how a server reports
    // a failed login.
    absorbCookies(response, url);

    // A 304 is not a response with an empty body, it is a verdict on the request
    // that carried the validators. Handling it here keeps the rest of the method
    // about ordinary responses.
    if (response.statusCode == 304) {
        handleNotModified(url, response);
        return;
    }

    Resource resource;
    resource.requestedUrl = url;
    resource.url = response.finalUrl.isValid() ? response.finalUrl : url;
    resource.statusCode = response.statusCode;
    resource.mimeType = response.contentType();

    if (response.isError()) {
        resource.error = tr("HTTP %1").arg(response.statusText());
    } else {
        resource.data = response.body;
        if (resource.mimeType.isEmpty())
            resource.mimeType = QStringLiteral("application/octet-stream");
    }

    // The response's own headers decide whether it may be kept, so an ordinary
    // response is not stored here - absorbIntoCache() reads Cache-Control and can
    // refuse. An error is stored only as a memo that must be revalidated, which
    // store() records, so a broken URL is not retried for every element.
    if (!absorbIntoCache(url, response, resource))
        store(resource);

    emit finished(resource);
}

void ResourceLoader::handleNotModified(const Url &url, const HttpResponse &response)
{
    ++m_revalidations;

    const auto it = m_cache.constFind(url.toString());
    if (it == m_cache.constEnd()) {
        // A 304 the cache cannot satisfy means the entry was dropped while the
        // request was in flight, which a reload can do. There is no body to serve
        // and inventing one would be worse than saying so.
        Resource resource;
        resource.requestedUrl = url;
        resource.url = url;
        resource.statusCode = 304;
        resource.error = tr("The server reported the resource as unchanged, but it is not "
                            "cached. Reload the page.");
        store(resource);
        emit finished(resource);
        return;
    }

    CacheEntry entry = it.value();

    // The stored body is served, but the response's own headers are allowed to
    // update the entry: a 304 may carry a new Date, a new Expires, or a new
    // Cache-Control, and ignoring those would make the entry revalidate on every
    // request from then on.
    const CachePolicy::Directives directives = CachePolicy::parse(response.headers);

    if (const QDateTime date = CachePolicy::parseHttpDate(response.header(QStringLiteral("Date")));
        date.isValid()) {
        entry.date = date;
    }
    if (const QDateTime expires
        = CachePolicy::parseHttpDate(response.header(QStringLiteral("Expires")));
        expires.isValid()) {
        entry.expires = expires;
    }
    // A validator may have been rotated, and the new one is what the next
    // revalidation has to send.
    if (const QString etag = response.header(QStringLiteral("ETag")); !etag.isEmpty())
        entry.etag = etag;

    // A 304 is the origin saying "what you hold is still current", so the clock
    // restarts from now. The lifetime is then recomputed from the merged headers,
    // which is what lets a 304 refresh the entry instead of leaving it to
    // revalidate again on the very next request.
    entry.storedAt = QDateTime::currentDateTimeUtc();
    if (directives.noCache)
        entry.noCache = true;
    entry.freshnessLifetime = CachePolicy::freshnessLifetime(directives, entry.date,
                                                            entry.expires, entry.lastModified);
    // With no lifetime information at all, a confirmed entry is treated as fresh
    // until its validators are used again. Leaving the lifetime at 0 would make
    // every subsequent request a revalidation, which is a round trip the 304 just
    // proved unnecessary.
    if (entry.freshnessLifetime <= 0 && !entry.noCache)
        entry.freshnessLifetime = kConfirmedFreshnessSeconds;

    entry.generation = m_generation;
    m_cache.insert(url.toString(), entry);

    emit finished(entry.resource);
}

void ResourceLoader::handleFailure(const Url &url, const QString &error)
{
    // A failed reload must not leave the marker behind: the next ordinary fetch
    // of this URL would then bypass the cache for no reason.
    m_reloadUrls.remove(url.toString());

    Resource resource;
    resource.requestedUrl = url;
    resource.url = url;
    resource.error = error;
    store(resource);
    emit finished(resource);
}

Resource ResourceLoader::loadLocalFile(const Url &url)
{
    Resource resource;
    resource.url = url;

    const QString path = url.toLocalFile();
    QFileInfo info(path);
    if (!info.exists()) {
        resource.error = QObject::tr("File not found: %1").arg(path);
        return resource;
    }
    if (info.isDir()) {
        resource.error = QObject::tr("%1 is a directory, not a file").arg(path);
        return resource;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        resource.error = QObject::tr("Cannot open %1: %2").arg(path, file.errorString());
        return resource;
    }

    resource.data = file.readAll();
    resource.statusCode = 200;

    QMimeDatabase database;
    resource.mimeType = database.mimeTypeForFile(info).name();
    if (resource.mimeType.isEmpty())
        resource.mimeType = QStringLiteral("application/octet-stream");

    return resource;
}

} // namespace oqb::network
