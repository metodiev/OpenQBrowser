#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

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

    /// Adds the `Cookie` header for `request` from the jar, and refuses to add
    /// one the security policy would block.
    void applyCookies(HttpRequest *request, const Url &referrer) const;
    /// Stores the cookies a response set.
    void absorbCookies(const HttpResponse &response, const Url &url);

    QHash<QString, Resource> m_cache;
    QList<PendingRequest> m_queue;
    QHash<HttpClient *, Url> m_clients;
    int m_maxConcurrent = 6;
    int m_requestTimeoutMs = 30000;
    int m_requestCount = 0;
    /// Not owned. Null means cookies are disabled for this loader.
    storage::CookieJar *m_cookies = nullptr;
    QString m_userAgent;
};

} // namespace oqb::network
