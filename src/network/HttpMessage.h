#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

#include "network/Url.h"

namespace oqb::network {

/// Ordered, case-insensitive header list.
///
/// HTTP header names are case-insensitive but their order and casing are worth
/// preserving for display in the DevTools inspector, so a list is used rather
/// than a map.
class HeaderList
{
public:
    using Entry = QPair<QString, QString>;

    void append(const QString &name, const QString &value);
    void set(const QString &name, const QString &value);
    void remove(const QString &name);

    bool contains(const QString &name) const;
    QString value(const QString &name, const QString &defaultValue = {}) const;
    QList<QString> values(const QString &name) const;

    /// Comma-separated values collapsed into a single string.
    QString joined(const QString &name) const;

    int size() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }
    const QList<Entry> &entries() const { return m_entries; }
    Entry at(int index) const { return m_entries.value(index); }

    void clear() { m_entries.clear(); }

private:
    QList<Entry> m_entries;
};

/// What the browser asks the server for.
class HttpRequest
{
public:
    enum class Method { Get, Post, Head };

    Method method = Method::Get;
    Url url;
    HeaderList headers;
    QByteArray body;
    /// Maximum number of redirects the client may follow for this request.
    int maxRedirects = 20;
    /// Milliseconds before the request is abandoned; 0 disables the timeout.
    int timeoutMs = 30000;
    /// Value used for the User-Agent header.
    QString userAgent;

    static QString methodName(Method method);

    /// The request as it goes on the wire (CRLF line endings, no body for HEAD).
    QByteArray serialize() const;
};

/// What the server sends back.
class HttpResponse
{
public:
    int statusCode = 0;
    QString reasonPhrase;
    QString httpVersion = QStringLiteral("HTTP/1.1");
    HeaderList headers;
    QByteArray body;

    /// URL the response was ultimately read from, after redirects.
    Url finalUrl;

    bool isValid() const { return statusCode > 0; }
    bool isSuccess() const { return statusCode >= 200 && statusCode < 300; }
    bool isRedirect() const
    {
        return statusCode == 301 || statusCode == 302 || statusCode == 303
            || statusCode == 307 || statusCode == 308;
    }
    bool isError() const { return statusCode >= 400; }

    QString header(const QString &name, const QString &defaultValue = {}) const
    {
        return headers.value(name, defaultValue);
    }

    /// The MIME type without parameters, lower-cased.
    QString contentType() const;

    /// The charset parameter, or an empty string when the server omitted it.
    QString charset() const;

    /// Decodes the body to text using the declared charset (UTF-8 by default).
    QString text() const;

    /// Absolute URL of the Location header, resolved against the final URL.
    Url location() const;

    /// Human readable summary used by error pages and the inspector.
    QString statusText() const;

    static QString reasonForStatus(int statusCode);
};

/// Helpers shared by the client and the tests.
namespace http {

/// Applies the `Content-Encoding` transforms (identity, gzip, deflate).
/// Returns an empty optional-like pair: `first` is success, `second` the body.
QPair<bool, QByteArray> decodeContentEncoding(const QString &encoding,
                                              const QByteArray &body);

/// Decodes `Transfer-Encoding: chunked` payloads.
QPair<bool, QByteArray> decodeChunked(const QByteArray &payload);

/// Parses a `text/html; charset=utf-8` style content type into its parts.
QPair<QString, QString> splitContentType(const QString &value);

/// Maps a charset label to a codec name Qt understands, e.g. "latin1".
QString codecNameForCharset(const QString &charset);

/// The default User-Agent used by OpenQBrowser.
QString defaultUserAgent();

/// Registers a CA bundle with Qt's default TLS configuration so that HTTPS
/// works on systems where Qt cannot locate the trust store itself.
bool installSystemCaCertificates();

} // namespace http

} // namespace oqb::network
