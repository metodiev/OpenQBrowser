#include "network/ResourceLoader.h"

#include "security/SecurityPolicy.h"
#include "storage/Cookies.h"

#include "network/HttpClient.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStringDecoder>

namespace oqb::network {

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
        *out = it.value();
    return true;
}

void ResourceLoader::store(const Resource &resource)
{
    m_cache.insert(resource.url.toString(), resource);
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

    if (Resource hit; cached(url, &hit)) {
        emit finished(hit);
        return;
    }

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
    // Another request for the same URL may have completed while this one waited.
    if (Resource hit; cached(request.url, &hit)) {
        emit finished(hit);
        return;
    }

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

    store(resource);

    // A permanent redirect makes the target authoritative for the old URL too,
    // so later lookups by the original URL resolve without another round trip.
    if ((response.statusCode == 301 || response.statusCode == 308)
        && resource.url != url) {
        Resource alias = resource;
        alias.url = url;
        store(alias);
    }

    emit finished(resource);
}

void ResourceLoader::handleFailure(const Url &url, const QString &error)
{
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
