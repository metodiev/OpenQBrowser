#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "network/HttpMessage.h"
#include "network/Url.h"

namespace oqb::network {

class HttpClient;

/// A resource fetched (or read from disk) on behalf of a page.
struct Resource
{
    Url url;
    QByteArray data;
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
    void clearCache() { m_cache.clear(); }

    int cacheSize() const { return m_cache.size(); }
    void setMaxConcurrentRequests(int count) { m_maxConcurrent = qMax(1, count); }
    int requestCount() const { return m_requestCount; }

    void setUserAgent(const QString &userAgent) { m_userAgent = userAgent; }

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

    QHash<QString, Resource> m_cache;
    QList<PendingRequest> m_queue;
    QHash<HttpClient *, Url> m_clients;
    int m_maxConcurrent = 6;
    int m_requestTimeoutMs = 30000;
    int m_requestCount = 0;
    QString m_userAgent;
};

} // namespace oqb::network
