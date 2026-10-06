#include "network/ResourceLoader.h"

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

    HttpRequest httpRequest;
    httpRequest.url = request.url;
    httpRequest.userAgent = m_userAgent;
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
