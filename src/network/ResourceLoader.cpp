#include "network/ResourceLoader.h"

#include "network/FetchPolicy.h"
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
    : ScriptFetchProvider(parent)
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

    // A script request that was cancelled must still settle: a promise that never
    // resolves is worse than one that fails, because the page waits for it
    // forever. Its own map is emptied into failures before the abort.
    const auto scriptClients = m_scriptClients;
    m_scriptClients.clear();
    for (auto it = scriptClients.constBegin(); it != scriptClients.constEnd(); ++it) {
        failScriptRequest(it.value(), tr("The request was cancelled"));
    }

    m_scriptQueue.clear();
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

    // Script requests wait in their own queue and share the same concurrency
    // budget, so a page that issues a hundred fetches cannot starve the images
    // and stylesheets the page itself needs.
    while (!m_scriptQueue.isEmpty() && m_clients.size() < m_maxConcurrent) {
        const auto entry = m_scriptQueue.takeFirst();
        startScriptRequest(entry.first, entry.second);
    }
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

/// Wires a client's completion back to the loader, whoever asked for the request.
///
/// Shared by both request paths on purpose. The script path used to build its own
/// client and connect nothing, so its responses arrived and were dropped: the page
/// waited on a promise that could never settle, and the dump never finished.
void ResourceLoader::connectClient(HttpClient *client)
{
    connect(client, &HttpClient::finished, this, [this, client](const HttpResponse &response) {
        // Script requests share this client plumbing but are reported on their
        // own signal, so the check comes first: a page's handler must never see
        // a fetch() response.
        if (const ScriptMeta meta = m_scriptClients.take(client); meta.requestId != 0) {
            client->deleteLater();
            handleScriptResponse(meta, response);
            pump();
            return;
        }

        const Url url = m_clients.value(client);
        m_clients.remove(client);
        client->deleteLater();
        handleHttpResponse(url, response);
        pump();
    });

    connect(client, &HttpClient::failed, this, [this, client](const QString &error) {
        if (const ScriptMeta meta = m_scriptClients.take(client); meta.requestId != 0) {
            client->deleteLater();
            failScriptRequest(meta, error);
            pump();
            return;
        }

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
                if (const ScriptMeta meta = m_scriptClients.value(client); meta.requestId != 0) {
                    absorbCookies(response, meta.url);
                    emit fetchRedirected(meta.requestId, redirectResponse(meta, response));
                    return;
                }
                absorbCookies(response, m_clients.value(client));
            });
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
    connectClient(client);

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
    resource.statusText = response.statusText();
    resource.mimeType = response.contentType();
    resource.headers = response.headers;

    // A response that carried its own reason phrase keeps it; one that did not
    // gets the standard text for its code, so `response.statusText` is never
    // empty for a status a browser would name.
    if (resource.statusText.isEmpty())
        resource.statusText = FetchPolicy::statusTextFor(response.statusCode);

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

// ------------------------------------------------------ requests from script

namespace {

/// Maps fetch()'s `credentials` names onto the enum the loader works with.
bool credentialsInclude(const QString &mode)
{
    return mode.compare(QLatin1String("include"), Qt::CaseInsensitive) == 0;
}

bool credentialsOmit(const QString &mode)
{
    return mode.compare(QLatin1String("omit"), Qt::CaseInsensitive) == 0;
}

} // namespace

int ResourceLoader::startScriptFetch(const ScriptRequest &request)
{
    m_scriptRefusal.clear();

    const Url url = Url::parse(request.url);
    if (!url.isValid()) {
        m_scriptRefusal = tr("the URL is not valid");
        return 0;
    }

    if (!FetchPolicy::isAllowedMethod(request.method)) {
        m_scriptRefusal = tr("%1 is not a method a page may use").arg(request.method);
        return 0;
    }

    const Url documentUrl = request.documentUrl.isEmpty() ? Url() : Url::parse(request.documentUrl);

    // The policy is consulted before a request is built, so a secure page cannot
    // be made to pull a plaintext resource in and a host the user reached over
    // HTTPS is not silently downgraded. This is the check the page's own
    // subresources already go through; script was the one way around it.
    const security::SecurityPolicy policy;
    if (const security::Decision decision = policy.canLoadSubresource(documentUrl, url);
        !decision.allowed) {
        m_scriptRefusal = decision.reason;
        return 0;
    }

    ScriptMeta meta;
    meta.requestId = m_nextScriptRequestId++;
    meta.url = url;
    meta.requestOrigin = documentUrl.serialisedOrigin();
    meta.sameOrigin = security::SecurityPolicy::sameOrigin(documentUrl, url);
    meta.withCredentials = credentialsInclude(request.credentials);
    meta.noCredentials = credentialsOmit(request.credentials);
    meta.length = static_cast<int>(request.body.size());

    ScriptRequest resolved = request;
    resolved.url = url.toString();
    resolved.documentUrl = documentUrl.toString();

    // A local file is read without a socket, so it completes here rather than
    // through the queue. It is still reported asynchronously, because a promise
    // that settled inside the call that created it would run its handler before
    // the page could attach one.
    if (url.isLocalFile()) {
        const Resource file = loadLocalFile(url);
        QMetaObject::invokeMethod(
            this,
            [this, meta, file] {
                emit fetchFinished(meta.requestId, scriptResponseFromResource(file, meta));
            },
            Qt::QueuedConnection);
        return meta.requestId;
    }

    if (!url.isRemote()) {
        QMetaObject::invokeMethod(
            this,
            [this, meta] {
                ScriptFetchResponse response;
                response.url = meta.url.toString();
                response.error = tr("OpenQBrowser cannot load %1 resources").arg(meta.url.scheme());
                emit fetchFinished(meta.requestId, response);
            },
            Qt::QueuedConnection);
        return meta.requestId;
    }

    ++m_requestCount;
    m_scriptQueue.append({meta, resolved});
    pump();
    return meta.requestId;
}

void ResourceLoader::abortScriptFetch(int requestId)
{
    // A queued request is dropped outright; one that is already in flight has its
    // client closed. Either way the id is forgotten, so nothing is reported for
    // it afterwards, which is what `AbortController` relies on.
    for (int i = 0; i < m_scriptQueue.size(); ++i) {
        if (m_scriptQueue.at(i).first.requestId == requestId) {
            m_scriptQueue.removeAt(i);
            return;
        }
    }

    for (auto it = m_scriptClients.begin(); it != m_scriptClients.end(); ++it) {
        if (it.value().requestId != requestId)
            continue;

        HttpClient *client = it.key();
        m_scriptClients.erase(it);
        m_clients.remove(client);
        client->abort();
        client->deleteLater();
        return;
    }
}

ScriptFetchResponse ResourceLoader::scriptResponseFromResource(const Resource &resource,
                                                               const ScriptMeta &meta)
{
    ScriptFetchResponse response;
    response.status = resource.statusCode;
    response.statusText = resource.statusText;
    response.body = resource.data;
    response.url = resource.url.isValid() ? resource.url.toString() : meta.url.toString();
    response.error = resource.error;

    for (const HeaderList::Entry &entry : resource.headers.entries())
        response.headers.append({entry.first, entry.second});

    return response;
}

void ResourceLoader::startScriptRequest(const ScriptMeta &meta, const ScriptRequest &request)
{
    const Url url = Url::parse(request.url);
    const Url documentUrl
        = request.documentUrl.isEmpty() ? Url() : Url::parse(request.documentUrl);

    // A GET that the cache can answer is answered from it: a page polling an
    // endpoint should not cost a request every time. Only a GET is served this
    // way, because a POST is not idempotent and a cached one would report a write
    // that never happened.
    const bool cacheable = request.method.compare(QLatin1String("GET"), Qt::CaseInsensitive) == 0;
    if (cacheable) {
        const auto it = m_cache.constFind(url.toString());
        if (it != m_cache.constEnd() && reusable(it.value(), QDateTime::currentDateTimeUtc())) {
            ++m_cacheHits;
            Resource stored = it.value().resource;

            // The entry may have been stored by a page request, so it carries
            // whatever headers that had. The CORS decision is still applied: the
            // same body can be perfectly readable by the page and unreadable by
            // script on another origin.
            if (!meta.sameOrigin) {
                const FetchPolicy::CorsHeaders cors
                    = FetchPolicy::CorsHeaders::fromHeaders(stored.headers);
                const FetchPolicy::CorsResult decision
                    = FetchPolicy::checkReadable(cors, meta.requestOrigin, meta.withCredentials);
                if (!decision.readable) {
                    stored.data.clear();
                    stored.error = tr("Cross-origin response blocked: %1").arg(decision.reason);
                } else {
                    stored.headers = FetchPolicy::crossOriginReadableHeaders(
                        stored.headers, cors.allowOrigin, cors.exposeHeaders,
                        meta.withCredentials);
                }
            }

            emit fetchFinished(meta.requestId, scriptResponseFromResource(stored, meta));
            return;
        }
    }

    auto *client = new HttpClient(this);
    m_scriptClients.insert(client, meta);
    // The client map is shared with the page path, which is what lets the one
    // completion lambda tell the two kinds apart and keeps the concurrency cap
    // covering both.
    m_clients.insert(client, url);
    connectClient(client);

    HttpRequest httpRequest;
    httpRequest.url = url;
    httpRequest.userAgent = m_userAgent;
    httpRequest.timeoutMs = m_requestTimeoutMs;
    httpRequest.body = request.body;

    for (const auto &header : request.headers)
        httpRequest.headers.append(header.first, header.second);

    if (request.method.compare(QLatin1String("GET"), Qt::CaseInsensitive) == 0)
        httpRequest.method = HttpRequest::Method::Get;
    else if (request.method.compare(QLatin1String("HEAD"), Qt::CaseInsensitive) == 0)
        httpRequest.method = HttpRequest::Method::Head;
    else
        httpRequest.method = HttpRequest::Method::Post;

    // The browser's own headers are set after the page's, so a page cannot
    // override them. FetchPolicy refuses the names a page may not set at all; this
    // is what makes that refusal hold for the ones a page may set to another
    // value.
    if (!httpRequest.body.isEmpty()
        && !httpRequest.headers.contains(QStringLiteral("Content-Type"))) {
        httpRequest.headers.set(QStringLiteral("Content-Type"),
                                QStringLiteral("text/plain;charset=UTF-8"));
    }
    if (url.isRemote())
        httpRequest.headers.set(QStringLiteral("Origin"), meta.requestOrigin);
    if (documentUrl.isValid())
        httpRequest.headers.set(QStringLiteral("Referer"), documentUrl.toString());

    if (!meta.noCredentials) {
        // The jar is consulted per hop, so a redirect that sets a cookie has it
        // sent on the hop that follows, exactly as the page path does.
        client->setCookieProvider([this, documentUrl](const Url &hop) {
            HttpRequest probe;
            probe.url = hop;
            applyCookies(&probe, documentUrl);
            return probe.headers.joined(QStringLiteral("Cookie"));
        });
        applyCookies(&httpRequest, documentUrl);
    } else {
        // A request that asked for no credentials must not have the ones a
        // redirect machinery would otherwise add back on a later hop.
        client->setCookieProvider([](const Url &) { return QString(); });
    }

    QMetaObject::invokeMethod(
        client, [client, httpRequest] { client->send(httpRequest); }, Qt::QueuedConnection);
}

void ResourceLoader::handleScriptResponse(const ScriptMeta &meta, const HttpResponse &response)
{
    absorbCookies(response, meta.url);

    Resource resource;
    resource.requestedUrl = meta.url;
    resource.url = response.finalUrl.isValid() ? response.finalUrl : meta.url;
    resource.statusCode = response.statusCode;
    resource.statusText = response.statusText();
    if (resource.statusText.isEmpty())
        resource.statusText = FetchPolicy::statusTextFor(response.statusCode);
    resource.headers = response.headers;
    resource.mimeType = response.contentType();
    resource.data = response.body;

    // The CORS decision is made on the response the page actually receives, not
    // on the first hop: after a redirect it is the final answer that has to grant
    // access.
    if (!meta.sameOrigin) {
        const FetchPolicy::CorsHeaders cors = FetchPolicy::CorsHeaders::fromHeaders(response.headers);
        const FetchPolicy::CorsResult decision
            = FetchPolicy::checkReadable(cors, meta.requestOrigin, meta.withCredentials);
        if (!decision.readable) {
            // The body is dropped rather than reported with a flag saying not to
            // read it. A check a page could ignore would be no protection at all,
            // so the bytes never reach script.
            resource.data.clear();
            resource.error = tr("Cross-origin response blocked: %1").arg(decision.reason);
        } else {
            // A response's headers describe more than its content - a request id,
            // an internal host name - so a cross-origin response exposes only the
            // ones it was willing to name. Filtering here rather than in the
            // bindings keeps the decision with the rest of the CORS rules.
            resource.headers = FetchPolicy::crossOriginReadableHeaders(
                response.headers, cors.allowOrigin, cors.exposeHeaders, meta.withCredentials);
        }
    }

    emit fetchFinished(meta.requestId, scriptResponseFromResource(resource, meta));
}

void ResourceLoader::failScriptRequest(const ScriptMeta &meta, const QString &error)
{
    ScriptFetchResponse response;
    response.url = meta.url.toString();
    response.error = error;
    emit fetchFinished(meta.requestId, response);
}

ScriptFetchResponse ResourceLoader::redirectResponse(const ScriptMeta &meta,
                                                    const HttpResponse &response) const
{
    Resource resource;
    resource.requestedUrl = meta.url;
    resource.url = response.finalUrl.isValid() ? response.finalUrl : meta.url;
    resource.statusCode = response.statusCode;
    resource.statusText = response.statusText();
    if (resource.statusText.isEmpty())
        resource.statusText = FetchPolicy::statusTextFor(response.statusCode);
    resource.headers = response.headers;
    resource.mimeType = response.contentType();

    ScriptFetchResponse out = scriptResponseFromResource(resource, meta);
    out.redirected = true;
    return out;
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
    resource.statusText = FetchPolicy::statusTextFor(200);

    QMimeDatabase database;
    resource.mimeType = database.mimeTypeForFile(info).name();
    if (resource.mimeType.isEmpty())
        resource.mimeType = QStringLiteral("application/octet-stream");

    // A file has no headers of its own, so the ones a server would have sent are
    // supplied. Without them a fetch() of a local file would see an empty Headers
    // object and a page branching on Content-Type would take the wrong path.
    resource.headers.set(QStringLiteral("Content-Type"), resource.mimeType);
    resource.headers.set(QStringLiteral("Content-Length"),
                         QString::number(resource.data.size()));

    return resource;
}

} // namespace oqb::network
