#include "network/FetchPolicy.h"

#include <QSet>

namespace oqb::network {

namespace {

/// A header name reduced to the form the tables below are keyed on. HTTP header
/// names are case-insensitive, but the checks must not depend on how the page
/// happened to spell them.
QString canonical(const QString &name)
{
    return name.trimmed().toLower();
}

/// The methods a page should never be able to choose. CONNECT opens a tunnel
/// through the browser, and TRACE and TRACK reflect the request back, which is
/// how a cross-site scripting filter used to be defeated.
bool isUniversallyUnsafeMethod(const QString &method)
{
    return method == QLatin1String("CONNECT") || method == QLatin1String("TRACE")
        || method == QLatin1String("TRACK");
}

/// True when `method` is a valid HTTP token: the characters RFC 7230 permits in
/// a method name. A method containing a space or a control character would not
/// survive being written into a request line, so it must be refused before the
/// request is built rather than after.
bool isToken(const QString &method)
{
    if (method.isEmpty())
        return false;

    for (const QChar c : method) {
        const ushort u = c.unicode();
        const bool isAlpha = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
        const bool isDigit = u >= u'0' && u <= u'9';
        // The punctuation RFC 7230 tchar allows, minus the separator characters
        // that would end the token early.
        const bool isPunctuation = u == u'!' || u == u'#' || u == u'$' || u == u'%' || u == u'&'
            || u == u'\'' || u == u'*' || u == u'+' || u == u'-' || u == u'.' || u == u'^'
            || u == u'_' || u == u'`' || u == u'|' || u == u'~';

        if (!isAlpha && !isDigit && !isPunctuation)
            return false;
    }
    return true;
}

/// The header names a page is never allowed to set, in lower case.
///
/// `Content-Length` and `Transfer-Encoding` would let a page describe a body
/// differently from the one it actually sent, which is request smuggling.
/// `Host` would let it address a different virtual host at the same address.
/// `Cookie`, `Origin` and `Referer` are set from the browser's own state, and
/// letting script write them would let it forge its own provenance or reach a
/// session it cannot otherwise read.
const QSet<QString> &forbiddenRequestHeaders()
{
    static const QSet<QString> kForbidden = {
        QStringLiteral("accept-charset"),
        QStringLiteral("accept-encoding"),
        QStringLiteral("access-control-request-headers"),
        QStringLiteral("access-control-request-method"),
        QStringLiteral("connection"),
        QStringLiteral("content-length"),
        QStringLiteral("cookie"),
        QStringLiteral("cookie2"),
        QStringLiteral("date"),
        QStringLiteral("dnt"),
        QStringLiteral("expect"),
        QStringLiteral("host"),
        QStringLiteral("keep-alive"),
        QStringLiteral("origin"),
        QStringLiteral("referer"),
        QStringLiteral("te"),
        QStringLiteral("trailer"),
        QStringLiteral("transfer-encoding"),
        QStringLiteral("upgrade"),
        QStringLiteral("via"),
    };
    return kForbidden;
}

/// The headers a request may carry and still count as simple. `Content-Type` is
/// here because it is checked further: only three values are harmless enough not
/// to need a preflight.
const QSet<QString> &safelistedRequestHeaderNames()
{
    static const QSet<QString> kSafelisted = {
        QStringLiteral("accept"),
        QStringLiteral("accept-language"),
        QStringLiteral("content-language"),
        QStringLiteral("content-type"),
    };
    return kSafelisted;
}

/// The response headers a cross-origin response exposes without naming them.
const QSet<QString> &safelistedResponseHeaderNames()
{
    static const QSet<QString> kSafelisted = {
        QStringLiteral("cache-control"),
        QStringLiteral("content-language"),
        QStringLiteral("content-length"),
        QStringLiteral("content-type"),
        QStringLiteral("expires"),
        QStringLiteral("last-modified"),
        QStringLiteral("pragma"),
    };
    return kSafelisted;
}

/// A header value long enough to be worth a preflight. The limit is the one the
/// Fetch standard sets, and its purpose is to stop a simple request being used as
/// a channel for smuggling a large payload through a header.
constexpr int kMaxSafelistedValueLength = 128;

bool isSafelistedContentType(const QString &value)
{
    // The MIME type alone decides, so the parameters are dropped first. A charset
    // does not change what the value means to a server.
    const int semicolon = value.indexOf(u';');
    const QString type = (semicolon >= 0 ? value.left(semicolon) : value).trimmed().toLower();

    return type == QLatin1String("application/x-www-form-urlencoded")
        || type == QLatin1String("multipart/form-data") || type == QLatin1String("text/plain");
}

} // namespace

bool FetchPolicy::isAllowedMethod(const QString &method)
{
    if (!isToken(method))
        return false;
    return !isUniversallyUnsafeMethod(method.toUpper());
}

bool FetchPolicy::isSimpleMethod(const QString &method)
{
    const QString upper = method.toUpper();
    return upper == QLatin1String("GET") || upper == QLatin1String("HEAD")
        || upper == QLatin1String("POST");
}

bool FetchPolicy::isForbiddenRequestHeader(const QString &name)
{
    const QString key = canonical(name);
    if (forbiddenRequestHeaders().contains(key))
        return true;

    // The protocol reserves these prefixes for the browser and the transport.
    // `Proxy-` would reach a proxy the user configured, and `Sec-` names the
    // headers that carry the browser's own security decisions.
    return key.startsWith(QLatin1String("proxy-")) || key.startsWith(QLatin1String("sec-"));
}

bool FetchPolicy::isSafelistedRequestHeader(const QString &name, const QString &value)
{
    const QString key = canonical(name);
    if (!safelistedRequestHeaderNames().contains(key))
        return false;

    // The value must be short enough that a server cannot be made to accept a
    // meaningful payload through it.
    if (value.toUtf8().size() > kMaxSafelistedValueLength)
        return false;

    // A value with a newline in it is a header-injection attempt; the client
    // would refuse to send it, but refusing here says so before a request is
    // queued.
    if (value.contains(u'\r') || value.contains(u'\n'))
        return false;

    if (key == QLatin1String("content-type"))
        return isSafelistedContentType(value);

    return true;
}

bool FetchPolicy::isSimpleRequest(const QString &method, const HeaderList &headers)
{
    if (!isSimpleMethod(method))
        return false;

    // A forbidden name makes the request non-simple as well as being refused
    // outright. Reporting it here keeps the two checks from disagreeing.
    for (const HeaderList::Entry &entry : headers.entries()) {
        if (isForbiddenRequestHeader(entry.first))
            return false;
        if (!isSafelistedRequestHeader(entry.first, entry.second))
            return false;
    }

    return true;
}

bool FetchPolicy::isSafelistedResponseHeader(const QString &name)
{
    return safelistedResponseHeaderNames().contains(canonical(name));
}

bool FetchPolicy::exposesHeader(const QString &name, const QStringList &exposedHeaders,
                                bool credentialsIncluded)
{
    if (isSafelistedResponseHeader(name))
        return true;

    const QString key = canonical(name);
    for (const QString &exposed : exposedHeaders) {
        if (exposed.trimmed() == QLatin1String("*")) {
            // A wildcard exposes everything, but only for a request that carried
            // no credentials: a server that answered "*" has not decided to trust
            // any particular origin with the session, so naming no header means
            // naming none of the ones a session would make sensitive.
            if (!credentialsIncluded)
                return true;
            continue;
        }
        if (exposed.trimmed().toLower() == key)
            return true;
    }
    return false;
}

HeaderList FetchPolicy::crossOriginReadableHeaders(const HeaderList &headers,
                                                  const QString &allowOrigin,
                                                  const QStringList &exposedHeaders,
                                                  bool credentialsIncluded)
{
    // A wildcard is not a wildcard for a credentialed request, so it must not be
    // treated as "everything is exposed" here either.
    const QStringList effective = (allowOrigin == QLatin1String("*") && credentialsIncluded)
        ? QStringList()
        : exposedHeaders;

    HeaderList out;
    for (const HeaderList::Entry &entry : headers.entries()) {
        if (exposesHeader(entry.first, effective, credentialsIncluded))
            out.append(entry.first, entry.second);
    }
    return out;
}

FetchPolicy::CorsHeaders FetchPolicy::CorsHeaders::fromHeaders(const HeaderList &headers)
{
    CorsHeaders cors;
    cors.allowOrigin = headers.value(QStringLiteral("Access-Control-Allow-Origin")).trimmed();
    cors.allowCredentials
        = headers.value(QStringLiteral("Access-Control-Allow-Credentials")).trimmed().toLower()
        == QLatin1String("true");
    cors.exposeHeaders = splitHeaderList(
        headers.value(QStringLiteral("Access-Control-Expose-Headers")));
    return cors;
}

FetchPolicy::CorsResult FetchPolicy::checkReadable(const CorsHeaders &headers,
                                                  const QString &requestOrigin,
                                                  bool credentialsIncluded)
{
    if (headers.allowOrigin.isEmpty()) {
        return {false, QStringLiteral("the response did not grant access to this origin")};
    }

    if (headers.allowOrigin == QLatin1String("*")) {
        // A wildcard is an answer to an anonymous request. Once the request
        // carried the user's cookies, only a named origin is an answer: a server
        // that replies "*" has said it does not care who reads the response, and
        // that cannot be taken as permission to expose session-bound data.
        if (credentialsIncluded) {
            return {false,
                    QStringLiteral("a wildcard allow-origin cannot authorise a request that "
                                   "carried credentials")};
        }
        return {true, {}};
    }

    // The value is compared as an origin, so a trailing slash or a default port
    // spelled out makes it not match, which is what a browser does.
    if (headers.allowOrigin.compare(requestOrigin, Qt::CaseInsensitive) != 0) {
        return {false,
                QStringLiteral("the response granted access to a different origin (%1)")
                    .arg(headers.allowOrigin)};
    }

    // A named origin with credentials requires the explicit second header, so
    // that a server which echoes back whatever it is sent still has to opt in.
    if (credentialsIncluded && !headers.allowCredentials) {
        return {false,
                QStringLiteral("a request with credentials needs "
                               "Access-Control-Allow-Credentials")};
    }

    return {true, {}};
}

QStringList FetchPolicy::splitHeaderList(const QString &value)
{
    QStringList parts;
    for (const QString &part : value.split(u',')) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty())
            parts.append(trimmed);
    }
    return parts;
}

QString FetchPolicy::statusTextFor(int statusCode)
{
    return HttpResponse::reasonForStatus(statusCode);
}

} // namespace oqb::network
