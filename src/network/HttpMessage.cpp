#include "network/HttpMessage.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStringList>

#include <zlib.h>

namespace oqb::network {

// ---------------------------------------------------------------- HeaderList

void HeaderList::append(const QString &name, const QString &value)
{
    m_entries.append({name, value.trimmed()});
}

void HeaderList::set(const QString &name, const QString &value)
{
    remove(name);
    append(name, value);
}

void HeaderList::remove(const QString &name)
{
    for (int i = m_entries.size() - 1; i >= 0; --i) {
        if (m_entries.at(i).first.compare(name, Qt::CaseInsensitive) == 0)
            m_entries.removeAt(i);
    }
}

bool HeaderList::contains(const QString &name) const
{
    for (const Entry &entry : m_entries) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString HeaderList::value(const QString &name, const QString &defaultValue) const
{
    for (const Entry &entry : m_entries) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            return entry.second;
    }
    return defaultValue;
}

QList<QString> HeaderList::values(const QString &name) const
{
    QList<QString> out;
    for (const Entry &entry : m_entries) {
        if (entry.first.compare(name, Qt::CaseInsensitive) == 0)
            out.append(entry.second);
    }
    return out;
}

QString HeaderList::joined(const QString &name) const
{
    return values(name).join(QStringLiteral(", "));
}

// ---------------------------------------------------------------- HttpRequest

QString HttpRequest::methodName(Method method)
{
    switch (method) {
    case Method::Get:
        return QStringLiteral("GET");
    case Method::Post:
        return QStringLiteral("POST");
    case Method::Head:
        return QStringLiteral("HEAD");
    }
    return QStringLiteral("GET");
}

QByteArray HttpRequest::serialize() const
{
    QString head = methodName(method) + u' ' + url.toRequestTarget() + u' '
        + QStringLiteral("HTTP/1.1\r\n");

    HeaderList outgoing = headers;
    if (!outgoing.contains(QStringLiteral("Host")))
        outgoing.set(QStringLiteral("Host"), url.host() + (url.port() >= 0 ? u':' + QString::number(url.port()) : QString()));
    if (!outgoing.contains(QStringLiteral("User-Agent")))
        outgoing.set(QStringLiteral("User-Agent"),
                     userAgent.isEmpty() ? http::defaultUserAgent() : userAgent);
    if (!outgoing.contains(QStringLiteral("Accept")))
        outgoing.set(QStringLiteral("Accept"),
                     QStringLiteral("text/html,application/xhtml+xml,application/xml;q=0.9,"
                                    "image/png,image/jpeg,image/svg+xml,image/gif,*/*;q=0.8"));
    if (!outgoing.contains(QStringLiteral("Accept-Encoding")))
        outgoing.set(QStringLiteral("Accept-Encoding"), QStringLiteral("gzip, deflate"));
    if (!outgoing.contains(QStringLiteral("Connection")))
        outgoing.set(QStringLiteral("Connection"), QStringLiteral("close"));
    if (method == Method::Post && !outgoing.contains(QStringLiteral("Content-Length")))
        outgoing.set(QStringLiteral("Content-Length"), QString::number(body.size()));

    for (const HeaderList::Entry &entry : outgoing.entries())
        head += entry.first + QStringLiteral(": ") + entry.second + QStringLiteral("\r\n");

    head += QStringLiteral("\r\n");

    QByteArray out = head.toUtf8();
    if (method != Method::Head)
        out += body;
    return out;
}

// --------------------------------------------------------------- HttpResponse

QString HttpResponse::contentType() const
{
    return http::splitContentType(header(QStringLiteral("Content-Type"))).first;
}

QString HttpResponse::charset() const
{
    return http::splitContentType(header(QStringLiteral("Content-Type"))).second;
}

QString HttpResponse::text() const
{
    QString charset = this->charset();
    // Fall back to a <meta charset> sniff when the server was silent, which is
    // the behaviour the HTML standard prescribes for documents.
    if (charset.isEmpty() && contentType() == QLatin1String("text/html")) {
        static const QRegularExpression metaCharset(
            QStringLiteral("<meta[^>]+charset\\s*=\\s*[\"']?([a-zA-Z0-9_\\-]+)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = metaCharset.match(QString::fromLatin1(body.left(4096)));
        if (match.hasMatch())
            charset = match.captured(1);
    }

    const QString codec = http::codecNameForCharset(charset);
    if (codec.compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) == 0)
        return QString::fromUtf8(body);

    auto decoder = QStringDecoder(codec.toLatin1().constData());
    if (!decoder.isValid()) {
        // Unknown labels are treated as windows-1252, matching browser rules.
        auto fallback = QStringDecoder("Windows-1252");
        return fallback.decode(body);
    }
    return decoder.decode(body);
}

Url HttpResponse::location() const
{
    const QString value = header(QStringLiteral("Location"));
    if (value.isEmpty())
        return {};
    return finalUrl.resolved(value);
}

QString HttpResponse::statusText() const
{
    if (reasonPhrase.isEmpty())
        return QString::number(statusCode);
    return QString::number(statusCode) + u' ' + reasonPhrase;
}

QString HttpResponse::reasonForStatus(int statusCode)
{
    switch (statusCode) {
    case 200: return QStringLiteral("OK");
    case 201: return QStringLiteral("Created");
    case 204: return QStringLiteral("No Content");
    case 206: return QStringLiteral("Partial Content");
    case 301: return QStringLiteral("Moved Permanently");
    case 302: return QStringLiteral("Found");
    case 303: return QStringLiteral("See Other");
    case 304: return QStringLiteral("Not Modified");
    case 307: return QStringLiteral("Temporary Redirect");
    case 308: return QStringLiteral("Permanent Redirect");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 408: return QStringLiteral("Request Timeout");
    case 410: return QStringLiteral("Gone");
    case 429: return QStringLiteral("Too Many Requests");
    case 500: return QStringLiteral("Internal Server Error");
    case 502: return QStringLiteral("Bad Gateway");
    case 503: return QStringLiteral("Service Unavailable");
    case 504: return QStringLiteral("Gateway Timeout");
    default: break;
    }
    if (statusCode >= 100 && statusCode < 200)
        return QStringLiteral("Informational");
    if (statusCode >= 300 && statusCode < 400)
        return QStringLiteral("Redirection");
    if (statusCode >= 400 && statusCode < 500)
        return QStringLiteral("Client Error");
    if (statusCode >= 500)
        return QStringLiteral("Server Error");
    return {};
}

// --------------------------------------------------------------------- http

namespace http {

QPair<bool, QByteArray> decodeContentEncoding(const QString &encoding, const QByteArray &body)
{
    const QString normalized = encoding.trimmed().toLower();
    if (normalized.isEmpty() || normalized == QLatin1String("identity"))
        return {true, body};

    if (normalized == QLatin1String("gzip")) {
        z_stream stream{};
        // 16 + MAX_WBITS selects gzip framing.
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
            return {false, {}};

        QByteArray out;
        out.reserve(body.size() * 4);
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(body.constData()));
        stream.avail_in = static_cast<uInt>(body.size());

        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        int result = Z_OK;
        do {
            stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            result = inflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                inflateEnd(&stream);
                return {false, {}};
            }
            out.append(buffer.constData(), buffer.size() - stream.avail_out);
            // A truncated stream still yields everything decoded so far; that is
            // what browsers render when a connection is cut mid-response.
            if (result == Z_BUF_ERROR && stream.avail_in == 0)
                break;
        } while (result != Z_STREAM_END);

        inflateEnd(&stream);
        return {true, out};
    }

    if (normalized == QLatin1String("deflate") || normalized == QLatin1String("x-deflate")) {
        // Try zlib framing first, then raw deflate, as servers are inconsistent.
        for (const int windowBits : {MAX_WBITS, -MAX_WBITS}) {
            z_stream stream{};
            if (inflateInit2(&stream, windowBits) != Z_OK)
                continue;

            QByteArray out;
            out.reserve(body.size() * 4);
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(body.constData()));
            stream.avail_in = static_cast<uInt>(body.size());

            QByteArray buffer(64 * 1024, Qt::Uninitialized);
            int result = Z_OK;
            bool failed = false;
            do {
                stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
                stream.avail_out = static_cast<uInt>(buffer.size());
                result = inflate(&stream, Z_NO_FLUSH);
                if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                    failed = true;
                    break;
                }
                out.append(buffer.constData(), buffer.size() - stream.avail_out);
                if (result == Z_BUF_ERROR && stream.avail_in == 0)
                    break;
            } while (result != Z_STREAM_END);

            inflateEnd(&stream);
            if (!failed && result == Z_STREAM_END)
                return {true, out};
        }
        return {false, {}};
    }

    // Unknown encodings are passed through untouched.
    return {true, body};
}

QPair<bool, QByteArray> decodeChunked(const QByteArray &payload)
{
    QByteArray out;
    int position = 0;

    auto readLine = [&payload, &position](QByteArray *line) -> bool {
        const int end = payload.indexOf("\r\n", position);
        if (end < 0)
            return false;
        *line = payload.mid(position, end - position);
        position = end + 2;
        return true;
    };

    while (true) {
        QByteArray line;
        if (!readLine(&line))
            return {false, {}};

        const int semicolon = line.indexOf(';');
        if (semicolon >= 0)
            line = line.left(semicolon);

        bool ok = false;
        const qint64 size = line.trimmed().toLongLong(&ok, 16);
        if (!ok || size < 0)
            return {false, {}};

        if (size == 0) {
            // Consume trailer headers up to the empty line.
            while (true) {
                QByteArray trailer;
                if (!readLine(&trailer))
                    break;
                if (trailer.isEmpty())
                    break;
            }
            return {true, out};
        }

        if (position + size > payload.size())
            return {false, {}};
        out += payload.mid(position, static_cast<int>(size));
        position += static_cast<int>(size);

        QByteArray terminator;
        if (!readLine(&terminator))
            return {false, {}};
    }
}

QPair<QString, QString> splitContentType(const QString &value)
{
    const QStringList parts = value.split(u';', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {QString(), QString()};

    const QString type = parts.first().trimmed().toLower();
    QString charset;
    for (int i = 1; i < parts.size(); ++i) {
        const QString parameter = parts.at(i).trimmed();
        if (parameter.startsWith(QLatin1String("charset="), Qt::CaseInsensitive)) {
            charset = parameter.mid(8).trimmed();
            if (charset.size() >= 2 && charset.startsWith(u'"') && charset.endsWith(u'"'))
                charset = charset.mid(1, charset.size() - 2);
            charset = charset.trimmed();
        }
    }
    return {type, charset};
}

QString codecNameForCharset(const QString &charset)
{
    const QString label = charset.trimmed().toLower();
    if (label.isEmpty())
        return QStringLiteral("UTF-8");

    static const QHash<QString, QString> aliases = {
        {QStringLiteral("utf8"), QStringLiteral("UTF-8")},
        {QStringLiteral("utf-8"), QStringLiteral("UTF-8")},
        {QStringLiteral("latin1"), QStringLiteral("ISO-8859-1")},
        {QStringLiteral("latin-1"), QStringLiteral("ISO-8859-1")},
        {QStringLiteral("iso8859-1"), QStringLiteral("ISO-8859-1")},
        {QStringLiteral("iso-8859-1"), QStringLiteral("ISO-8859-1")},
        {QStringLiteral("windows-1252"), QStringLiteral("Windows-1252")},
        {QStringLiteral("cp1252"), QStringLiteral("Windows-1252")},
        {QStringLiteral("ascii"), QStringLiteral("UTF-8")},
        {QStringLiteral("us-ascii"), QStringLiteral("UTF-8")},
        {QStringLiteral("utf-16"), QStringLiteral("UTF-16")},
        {QStringLiteral("utf-16le"), QStringLiteral("UTF-16LE")},
        {QStringLiteral("utf-16be"), QStringLiteral("UTF-16BE")},
        {QStringLiteral("iso-8859-15"), QStringLiteral("ISO-8859-15")},
        {QStringLiteral("koi8-r"), QStringLiteral("KOI8-R")},
        {QStringLiteral("shift_jis"), QStringLiteral("Shift_JIS")},
        {QStringLiteral("euc-jp"), QStringLiteral("EUC-JP")},
        {QStringLiteral("gbk"), QStringLiteral("GBK")},
        {QStringLiteral("gb2312"), QStringLiteral("GBK")},
        {QStringLiteral("big5"), QStringLiteral("Big5")},
    };

    const auto it = aliases.constFind(label);
    if (it != aliases.constEnd())
        return it.value();
    return charset;
}

QString defaultUserAgent()
{
    return QStringLiteral("Mozilla/5.0 (compatible; OpenQBrowser/%1; +https://github.com/metodiev/OpenQBrowser)")
        .arg(QStringLiteral(OPENQBROWSER_VERSION));
}

bool installSystemCaCertificates()
{
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    if (!configuration.caCertificates().isEmpty())
        return true;

    static const QStringList candidates = {
        QStringLiteral("/etc/ssl/cert.pem"),
        QStringLiteral("/opt/homebrew/etc/ca-certificates/cert.pem"),
        QStringLiteral("/opt/homebrew/etc/openssl@3/cert.pem"),
        QStringLiteral("/usr/local/etc/ca-certificates/cert.pem"),
        QStringLiteral("/etc/pki/tls/certs/ca-bundle.crt"),
        QStringLiteral("/etc/ssl/certs/ca-certificates.crt"),
        QStringLiteral("/etc/ssl/ca-bundle.pem"),
    };

    for (const QString &path : candidates) {
        if (!QFileInfo::exists(path))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QList<QSslCertificate> certificates
            = QSslCertificate::fromData(file.readAll(), QSsl::Pem);
        if (certificates.isEmpty())
            continue;
        configuration.setCaCertificates(certificates);
        QSslConfiguration::setDefaultConfiguration(configuration);
        return true;
    }
    return false;
}

} // namespace http

} // namespace oqb::network
